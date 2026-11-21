#include "panes/ProjectExplorerPane.h"

#include "settings/SettingsDialog.h"
#include "shell/WindowChrome.h"
#include "theme/SpellIcons.h"
#include "theme/SpellTheme.h"
#include "widgets/SpellBadges.h"

#include <QAction>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEnterEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeySequence>
#include <QLabel>
#include <QLayout>
#include <QLayoutItem>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <functional>
#include <utility>

namespace spell {

namespace {

constexpr int kInfoLabelWidth = 56;
constexpr int kTickerMaxRows = 6;
constexpr int kMaxPushedNotices = 24;
constexpr std::array<double, kTickerMaxRows> kTickerOpacities = {
    1.0, 0.85, 0.73, 0.61, 0.49, 0.37
};

// -----------------------------------------------------------------------------
// Typography & color helpers
// -----------------------------------------------------------------------------

QFont makeUiFont(double pointSize, QFont::Weight weight = QFont::Normal) {
    QFont font = QGuiApplication::font();
    font.setPointSizeF(pointSize);
    font.setWeight(weight);
    return font;
}

QFont makeMonoFont(double pointSize, QFont::Weight weight = QFont::Normal) {
    QFont font(ThemeManager::instance().editorFontFamily());
    font.setStyleHint(QFont::Monospace);
    font.setPointSizeF(pointSize);
    font.setWeight(weight);
    return font;
}

void setLabelColor(QLabel* label, const QColor& color) {
    if (!label) {
        return;
    }
    label->setStyleSheet(
        QStringLiteral("QLabel { background-color: transparent; color: %1; }")
            .arg(qssColor(color)));
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, color);
    pal.setColor(QPalette::Text, color);
    label->setPalette(pal);
    label->update();
}

void clearLayout(QLayout* layout) {
    if (!layout) {
        return;
    }
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            // Detach immediately so findChildren(...) never sees pending deleteLater() zombies.
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        } else if (QLayout* childLayout = item->layout()) {
            clearLayout(childLayout);
        }
        delete item;
    }
}

// -----------------------------------------------------------------------------
// Single-line eliding label that never expands sidebar minimum width
// -----------------------------------------------------------------------------

class ElidedLabel final : public QLabel {
public:
    explicit ElidedLabel(const QString& text = QString(),
                         Qt::TextElideMode elideMode = Qt::ElideRight,
                         QWidget* parent = nullptr)
        : QLabel(text, parent)
        , m_elideMode(elideMode) {
        setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setStyleSheet(QStringLiteral("background-color: transparent;"));
    }

    void setElideMode(Qt::TextElideMode mode) {
        if (m_elideMode != mode) {
            m_elideMode = mode;
            update();
        }
    }

    void setTextColor(const QColor& color) {
        if (m_textColor != color) {
            m_textColor = color;
            QPalette pal = palette();
            pal.setColor(QPalette::WindowText, color);
            pal.setColor(QPalette::Text, color);
            setPalette(pal);
            update();
        }
    }

    QSize minimumSizeHint() const override {
        const QFontMetrics fm(font());
        return QSize(0, fm.height());
    }

    QSize sizeHint() const override {
        const QFontMetrics fm(font());
        return QSize(fm.horizontalAdvance(text()), fm.height());
    }

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.setPen(m_textColor.isValid() ? m_textColor : palette().color(foregroundRole()));
        painter.setFont(font());
        const QRect rect = contentsRect();
        const QString elided = painter.fontMetrics().elidedText(
            text(), m_elideMode, std::max(0, rect.width()));
        painter.drawText(rect, static_cast<int>(alignment() | Qt::TextSingleLine), elided);
    }

private:
    Qt::TextElideMode m_elideMode = Qt::ElideRight;
    QColor m_textColor;
};

void setElidedColor(ElidedLabel* label, const QColor& color) {
    if (label) {
        label->setTextColor(color);
    }
}

// -----------------------------------------------------------------------------
// Interactive rounded monochrome row widget (hover, selected fill, click, dbl-click, menu)
// -----------------------------------------------------------------------------

class InteractiveRowWidget final : public QWidget {
public:
    explicit InteractiveRowWidget(QWidget* parent = nullptr)
        : QWidget(parent) {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setContextMenuPolicy(Qt::CustomContextMenu);
        connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
            if (onContextMenu) {
                onContextMenu(mapToGlobal(pos));
            }
        });
    }

    void setSelected(bool selected) {
        if (m_selected != selected) {
            m_selected = selected;
            update();
        }
    }

    void setColors(const QColor& hoverFill, const QColor& selectedFill) {
        m_hoverFill = hoverFill;
        m_selectedFill = selectedFill;
        update();
    }

    void setCornerRadius(qreal radius) {
        m_radius = radius;
        update();
    }

    std::function<void()> onClick;
    std::function<void()> onDoubleClick;
    std::function<void(const QPoint& globalPos)> onContextMenu;

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        QColor fill;
        if (m_selected && m_selectedFill.isValid() && m_selectedFill.alpha() > 0) {
            fill = m_selectedFill;
        } else if (m_hovered && m_hoverFill.isValid() && m_hoverFill.alpha() > 0) {
            fill = m_hoverFill;
        }
        if (!fill.isValid() || fill.alpha() == 0) {
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(QRectF(rect()), m_radius, m_radius);
    }

    void enterEvent(QEnterEvent* event) override {
        m_hovered = true;
        update();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        m_hovered = false;
        update();
        QWidget::leaveEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event && event->button() == Qt::LeftButton) {
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event && event->button() == Qt::LeftButton) {
            if (m_ignoreNextRelease) {
                m_ignoreNextRelease = false;
                event->accept();
                return;
            }
            if (rect().contains(event->position().toPoint()) && onClick) {
                onClick();
            }
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event && event->button() == Qt::LeftButton && onDoubleClick) {
            m_ignoreNextRelease = true;
            onDoubleClick();
            event->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }

private:
    QColor m_hoverFill;
    QColor m_selectedFill;
    qreal m_radius = 5.0;
    bool m_selected = false;
    bool m_hovered = false;
    bool m_ignoreNextRelease = false;
};

// -----------------------------------------------------------------------------
// Formatting & extraction helpers
// -----------------------------------------------------------------------------

QString singleLine(const QString& raw) {
    QString out = raw;
    out.replace(QLatin1Char('\r'), QLatin1Char(' '));
    out.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return out.simplified();
}

qint64 normalizeEpochSeconds(qint64 rawTimestamp) {
    if (rawTimestamp <= 0) {
        return 0;
    }
    return rawTimestamp > 10000000000LL ? (rawTimestamp / 1000) : rawTimestamp;
}

QString formatTickerTime(qint64 epochSecs) {
    if (epochSecs <= 0) {
        return QStringLiteral("--:--");
    }
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(epochSecs);
    if (!dt.isValid()) {
        return QStringLiteral("--:--");
    }
    return dt.toLocalTime().toString(QStringLiteral("HH:mm"));
}

QString formatRelativeTimestamp(qint64 rawTimestamp) {
    const qint64 secs = normalizeEpochSeconds(rawTimestamp);
    if (secs <= 0) {
        return {};
    }
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(secs);
    if (!dt.isValid()) {
        return {};
    }
    return dt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString projectDisplayTitle(const ProjectInfo& project) {
    const QString trimmed = project.name.trimmed();
    if (!trimmed.isEmpty()) {
        return trimmed;
    }
    const QString cleaned = QDir::cleanPath(project.rootPath.trimmed());
    if (!cleaned.isEmpty()) {
        const QString base = QFileInfo(cleaned).fileName();
        if (!base.isEmpty()) {
            return base;
        }
        return cleaned;
    }
    return QStringLiteral("Project");
}

QString formatEndpointHost(const QString& rawUrl) {
    const QString trimmed = rawUrl.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    const QUrl parsed(trimmed);
    if (parsed.isValid() && !parsed.host().isEmpty()) {
        if (parsed.port() > 0) {
            return QStringLiteral("%1:%2").arg(parsed.host()).arg(parsed.port());
        }
        return parsed.host();
    }
    return trimmed;
}

QString compactPathForDisplay(const QString& fullPath) {
    const QString cleaned = QDir::cleanPath(fullPath);
    const QString home = QDir::cleanPath(QDir::homePath());
    if (!home.isEmpty() && cleaned.startsWith(home)) {
        if (cleaned.size() == home.size()) {
            return QStringLiteral("~");
        }
        if (cleaned.at(home.size()) == QLatin1Char('/')) {
            return QLatin1Char('~') + cleaned.mid(home.size());
        }
    }
    return cleaned;
}

QString settingsShortcutHint() {
    QString seq = QKeySequence(QKeySequence::Preferences).toString(QKeySequence::NativeText);
    if (seq.isEmpty()) {
        seq = QKeySequence(Qt::CTRL | Qt::Key_Comma).toString(QKeySequence::NativeText);
    }
    return seq.isEmpty() ? QStringLiteral("Settings") : QStringLiteral("Settings (%1)").arg(seq);
}

bool pathsEqual(const QString& a, const QString& b) {
    if (a.isEmpty() || b.isEmpty()) {
        return false;
    }
    const QString ca = QDir::cleanPath(a);
    const QString cb = QDir::cleanPath(b);
#ifdef Q_OS_WIN
    return QString::compare(ca, cb, Qt::CaseInsensitive) == 0;
#else
    return ca == cb;
#endif
}

template <typename T>
QString extractThreadLinkedFolder([[maybe_unused]] const T& thread) {
    if constexpr (requires(const T& t) { { t.linkedFolder } -> std::convertible_to<QString>; }) {
        return thread.linkedFolder;
    } else {
        return {};
    }
}

QHash<QString, QString> extractSnapshotThreadLinkedFolders(const WorkspaceSnapshot& snap) {
    QHash<QString, QString> result;
    const QJsonValue threadsVal = snap.raw.value(QStringLiteral("threads"));
    if (!threadsVal.isArray()) {
        return result;
    }
    const QJsonArray arr = threadsVal.toArray();
    for (const QJsonValue& item : arr) {
        if (!item.isObject()) {
            continue;
        }
        const QJsonObject obj = item.toObject();
        const QString id = detail::getString(obj, {"id", "thread_id", "threadId"}).trimmed();
        const QString folder = detail::getString(
            obj, {"linked_folder", "linkedFolder", "working_dir", "workingDir", "folder_path", "folderPath"}).trimmed();
        if (!id.isEmpty() && !folder.isEmpty()) {
            result.insert(id, QDir::cleanPath(folder));
        }
    }
    return result;
}

bool isArmedTimer(const AgentTimer& timer) {
    if (timer.delivered) {
        return false;
    }
    const QString st = timer.status.trimmed().toLower();
    return st != QLatin1StringView("cancelled")
        && st != QLatin1StringView("canceled")
        && st != QLatin1StringView("fired")
        && st != QLatin1StringView("delivered")
        && st != QLatin1StringView("completed");
}

QString truncateTarget(const QString& value) {
    const QString clean = singleLine(value);
    if (clean.size() > 48) {
        return clean.left(47) + QStringLiteral("…");
    }
    return clean;
}

QString extractToolTarget(const UiChatEvent& ev) {
    if (!ev.toolArgsJson.trimmed().isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(ev.toolArgsJson.toUtf8());
        if (doc.isObject()) {
            const QJsonObject obj = doc.object();
            static constexpr std::array<const char*, 6> kKeys = {
                "path", "command", "pattern", "url", "label", "name"
            };
            for (const char* key : kKeys) {
                const QJsonValue val = obj.value(QLatin1StringView(key));
                if (val.isString()) {
                    const QString s = val.toString().trimmed();
                    if (!s.isEmpty()) {
                        return truncateTarget(s);
                    }
                }
            }
        }
    }
    if (!ev.filePath.trimmed().isEmpty()) {
        return truncateTarget(ev.filePath.trimmed());
    }
    return {};
}

QString formatToolStatusWord(const QString& rawStatus, bool hasOutput) {
    const QString lower = rawStatus.trimmed().toLower();
    if (lower.isEmpty()) {
        return hasOutput ? QStringLiteral("done") : QStringLiteral("running");
    }
    if (lower == QLatin1StringView("awaiting_confirmation")) {
        return QStringLiteral("needs approval");
    }
    if (lower == QLatin1StringView("completed") ||
        lower == QLatin1StringView("ok") ||
        lower == QLatin1StringView("succeeded") ||
        lower == QLatin1StringView("success")) {
        return QStringLiteral("done");
    }
    QString readable = lower;
    readable.replace(QLatin1Char('_'), QLatin1Char(' '));
    return readable;
}

bool isErrorToolStatus(const QString& rawStatus) {
    const QString lower = rawStatus.trimmed().toLower();
    return lower == QLatin1StringView("denied")
        || lower == QLatin1StringView("failed")
        || lower == QLatin1StringView("error")
        || lower == QLatin1StringView("rejected");
}

QString formatCompactionNotice(const UiChatEvent& ev) {
    const QJsonObject advice = detail::getObject(ev.raw, {"context_advice", "contextAdvice"});
    if (!advice.isEmpty()) {
        const int epoch = detail::getInt(advice, {"compaction_epoch", "compactionEpoch"}, -1);
        const int freed = detail::getInt(advice, {"freed_blocks", "freedBlocks"}, -1);
        if (epoch >= 0 || freed >= 0) {
            return QStringLiteral("Context compacted · epoch %1, freed %2 blocks")
                .arg(std::max(0, epoch))
                .arg(std::max(0, freed));
        }
    }
    const QString body = singleLine(ev.text);
    if (!body.isEmpty()) {
        return QStringLiteral("Context compacted · %1").arg(body);
    }
    return QStringLiteral("Context compacted · epoch updated");
}

struct TickerEntry {
    QString id;
    qint64 timeSecs = 0;
    QString text;
    bool isError = false;

    bool operator==(const TickerEntry&) const = default;
};

struct PushedNotice {
    QString id;
    qint64 timeSecs = 0;
    quint64 seq = 0;
    QString text;
    bool isError = false;
};

QVector<TickerEntry> buildTickerEntries(const QVector<UiChatEvent>& events,
                                        const QVector<PushedNotice>& pushedNotices) {
    struct SortableEntry {
        TickerEntry entry;
        quint64 tieBreaker = 0;
    };

    QVector<SortableEntry> combined;
    combined.reserve(events.size() + pushedNotices.size());

    quint64 evOrder = 0;
    for (int i = 0; i < events.size(); ++i) {
        const UiChatEvent& ev = events.at(i);
        const qint64 timeSecs = normalizeEpochSeconds(ev.timestamp);
        const QString baseId = ev.id.isEmpty() ? QStringLiteral("ev-%1").arg(i) : ev.id;

        if (ev.kind == QLatin1StringView("compaction_notice")) {
            combined.append(SortableEntry{
                TickerEntry{baseId, timeSecs, formatCompactionNotice(ev), false},
                ++evOrder
            });
            continue;
        }

        if (ev.kind == QLatin1StringView("timer_alarm")) {
            const QString body = singleLine(ev.text);
            const QString msg = body.isEmpty()
                ? QStringLiteral("Timer · alarm fired")
                : QStringLiteral("Timer · %1").arg(body);
            combined.append(SortableEntry{
                TickerEntry{baseId, timeSecs, msg, false},
                ++evOrder
            });
            continue;
        }

        if (ev.kind == QLatin1StringView("error")) {
            const QString body = singleLine(ev.text);
            if (!body.isEmpty()) {
                combined.append(SortableEntry{
                    TickerEntry{baseId, timeSecs, body, true},
                    ++evOrder
                });
            }
            continue;
        }

        if (ev.kind == QLatin1StringView("tool_call") || !ev.toolName.trimmed().isEmpty()) {
            const QString toolName = ev.toolName.trimmed().isEmpty()
                ? QStringLiteral("tool")
                : ev.toolName.trimmed();
            QString rawStatus = ev.diffStatus.trimmed();
            if (rawStatus.isEmpty()) {
                rawStatus = detail::getString(ev.raw, {"status", "tool_status", "toolStatus"}).trimmed();
            }
            const QString target = extractToolTarget(ev);
            const QString statusWord = formatToolStatusWord(rawStatus, !ev.toolOutput.trimmed().isEmpty());
            const QString msg = target.isEmpty()
                ? QStringLiteral("%1 · %2").arg(toolName, statusWord)
                : QStringLiteral("%1 %2 · %3").arg(toolName, target, statusWord);
            combined.append(SortableEntry{
                TickerEntry{baseId + QLatin1Char(':') + rawStatus, timeSecs, msg, isErrorToolStatus(rawStatus)},
                ++evOrder
            });
        }

        if (ev.kind == QLatin1StringView("assistant")) {
            const QString trunc = detail::getString(
                ev.raw,
                {"effective_truncation_notice", "effectiveTruncationNotice",
                 "truncation_notice", "truncationNotice"}).trimmed();
            if (!trunc.isEmpty()) {
                combined.append(SortableEntry{
                    TickerEntry{baseId + QStringLiteral("-trunc"),
                                timeSecs,
                                QStringLiteral("Response hit the token limit"),
                                true},
                    ++evOrder
                });
            }
        }
    }

    constexpr quint64 kNoticeTieBase = 1000000ULL;
    for (const PushedNotice& notice : pushedNotices) {
        combined.append(SortableEntry{
            TickerEntry{notice.id, notice.timeSecs, notice.text, notice.isError},
            kNoticeTieBase + notice.seq
        });
    }

    std::stable_sort(combined.begin(),
                     combined.end(),
                     [](const SortableEntry& a, const SortableEntry& b) {
                         if (a.entry.timeSecs != b.entry.timeSecs) {
                             return a.entry.timeSecs < b.entry.timeSecs;
                         }
                         return a.tieBreaker < b.tieBreaker;
                     });

    QVector<TickerEntry> out;
    const int count = std::min<int>(kTickerMaxRows, static_cast<int>(combined.size()));
    out.reserve(count);
    for (int i = 0; i < count; ++i) {
        out.append(combined.at(combined.size() - 1 - i).entry);
    }
    return out;
}

} // namespace

// =============================================================================
// ProjectExplorerPane::Impl
// =============================================================================

struct ProjectExplorerPane::Impl {
    ProjectExplorerPane* owner = nullptr;

    // -------------------------------------------------------------------------
    // Top: SpellInfoSubpanel (sole status & notification area in Spell IDE)
    // -------------------------------------------------------------------------
    QWidget* infoSubpanel = nullptr;
    QWidget* infoTopBar = nullptr;
    QLabel* panelTitleLabel = nullptr;

    QWidget* infoRowsContainer = nullptr;
    QVector<QLabel*> infoKeyLabels;

    InteractiveRowWidget* serverRow = nullptr;
    ElidedLabel* serverValueLabel = nullptr;

    QWidget* modelRow = nullptr;
    ElidedLabel* modelValueLabel = nullptr;

    QWidget* messageRow = nullptr;
    ElidedLabel* messageValueLabel = nullptr;

    QWidget* tagsRow = nullptr;
    ElidedLabel* tagsValueLabel = nullptr;

    QWidget* timersRow = nullptr;
    ElidedLabel* timersValueLabel = nullptr;

    QWidget* terminalRow = nullptr;
    ElidedLabel* terminalValueLabel = nullptr;

    struct TickerRowUi {
        QWidget* row = nullptr;
        QLabel* timeLabel = nullptr;
        ElidedLabel* textLabel = nullptr;
    };
    QWidget* statusTickerWidget = nullptr;
    std::array<TickerRowUi, kTickerMaxRows> tickerRows{};

    // Dividers
    SpellHairline* topHairline = nullptr;
    SpellHairline* bottomHairline = nullptr;

    // -------------------------------------------------------------------------
    // Middle: Nested ProjectsSubpanel
    // -------------------------------------------------------------------------
    QWidget* projectsSubpanel = nullptr;
    QLabel* projectsSectionTitle = nullptr;
    SpellIconButton* addProjectBtn = nullptr;
    QStackedWidget* projectsStack = nullptr;

    QWidget* projectsEmptyPage = nullptr;

    QScrollArea* projectsScrollArea = nullptr;
    QWidget* projectsScrollContent = nullptr;
    QVBoxLayout* projectsListLayout = nullptr;

    struct ThreadRowUi {
        QString threadId;
        InteractiveRowWidget* row = nullptr;
        ElidedLabel* titleLabel = nullptr;
        ElidedLabel* folderLabel = nullptr;
        QLabel* folderIconLabel = nullptr;
    };

    struct ProjectSectionUi {
        QString projectId;
        InteractiveRowWidget* headerRow = nullptr;
        SpellIconButton* chevronBtn = nullptr;
        ElidedLabel* nameLabel = nullptr;
        QLabel* readOnlyIconLabel = nullptr;
        QWidget* conversationsContainer = nullptr;
        InteractiveRowWidget* newConversationRow = nullptr;
        QLabel* newConversationIcon = nullptr;
        QLabel* newConversationLabel = nullptr;
        QVector<ThreadRowUi> threadRows;
    };
    QVector<ProjectSectionUi> projectSectionUis;

    // -------------------------------------------------------------------------
    // Bottom: Minimal SpellSettingsSubpanel
    // -------------------------------------------------------------------------
    QWidget* settingsSubpanel = nullptr;
    QLabel* footerHatLabel = nullptr;
    SpellIconButton* settingsGearBtn = nullptr;

    // -------------------------------------------------------------------------
    // Cached state for minimal 30ms streaming diffing
    // -------------------------------------------------------------------------
    QVector<ProjectInfo> projects;
    QVector<ThreadSummary> allThreads;
    QVector<UiChatEvent> activeEvents;
    QString activeProjectId;
    QString activeThreadId;
    QSet<QString> expandedProjectIds;
    QHash<QString, QString> linkedFolders;
    QHash<QString, QString> snapshotThreadLinkedFolders;

    // Info panel cached values
    bool infoInitialized = false;
    bool endpointConfigured = false;
    bool engineOnline = false;
    QString endpointBaseUrl;
    QString engineError;
    QString modelId;
    bool isAgentBusy = false;
    double decodeTokS = 0.0;
    qint64 activePromptTokens = 0;
    QString activeTagsText;
    QString armedTimersText;
    int pendingTerminalApprovalsCount = 0;

    // Status ticker state
    QVector<PushedNotice> pushedNotices;
    quint64 nextNoticeSeq = 0;
    QVector<TickerEntry> renderedTickerEntries;

    // -------------------------------------------------------------------------
    // UI Construction
    // -------------------------------------------------------------------------

    void buildUi() {
        auto* rootLayout = new QVBoxLayout(owner);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(0);

        buildInfoSubpanel();
        rootLayout->addWidget(infoSubpanel, 0);

        topHairline = new SpellHairline(Qt::Horizontal, owner);
        rootLayout->addWidget(topHairline, 0);

        buildProjectsSubpanel();
        rootLayout->addWidget(projectsSubpanel, 1);

        bottomHairline = new SpellHairline(Qt::Horizontal, owner);
        rootLayout->addWidget(bottomHairline, 0);

        buildSettingsSubpanel();
        rootLayout->addWidget(settingsSubpanel, 0);
    }

    QLabel* makeInfoKeyLabel(const QString& text, QWidget* parent) {
        auto* lbl = new QLabel(text, parent);
        lbl->setFixedWidth(kInfoLabelWidth);
        lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        lbl->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        infoKeyLabels.append(lbl);
        return lbl;
    }

    void buildInfoSubpanel() {
        infoSubpanel = new QWidget(owner);
        infoSubpanel->setObjectName(QStringLiteral("spellInfoSubpanel"));
        infoSubpanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);

        auto* subLayout = new QVBoxLayout(infoSubpanel);
        subLayout->setContentsMargins(0, 0, 0, 0);
        subLayout->setSpacing(0);

        // Top bar (34px): left-aligned "BEATRIXLESLIE IDE" title (window buttons float on left on macOS)
        infoTopBar = new QWidget(infoSubpanel);
        infoTopBar->setObjectName(QStringLiteral("spellInfoTopBar"));
        infoTopBar->setFixedHeight(34);
        installWindowDragRegion(infoTopBar);
        auto* topBarLayout = new QHBoxLayout(infoTopBar);
#ifdef __APPLE__
        topBarLayout->setContentsMargins(78, 0, 14, 0);
#else
        topBarLayout->setContentsMargins(14, 0, 14, 0);
#endif
        topBarLayout->setSpacing(0);

        panelTitleLabel = new QLabel(QStringLiteral("BEATRIXLESLIE IDE"), infoTopBar);
        panelTitleLabel->setObjectName(QStringLiteral("spellExplorerTitleLabel"));
        panelTitleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        panelTitleLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        topBarLayout->addWidget(panelTitleLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
        topBarLayout->addStretch(1);
        subLayout->addWidget(infoTopBar);

        // Key-value info rows
        infoRowsContainer = new QWidget(infoSubpanel);
        auto* rowsLayout = new QVBoxLayout(infoRowsContainer);
        rowsLayout->setContentsMargins(10, 0, 10, 8);
        rowsLayout->setSpacing(2);

        // 1. Server (clickable -> opens Settings -> Engine)
        serverRow = new InteractiveRowWidget(infoRowsContainer);
        serverRow->setCornerRadius(4.0);
        serverRow->onClick = [this]() {
            emit owner->settingsRequested(static_cast<int>(SettingsPaneId::Engine));
        };
        {
            auto* l = new QHBoxLayout(serverRow);
            l->setContentsMargins(4, 1, 4, 1);
            l->setSpacing(5);
            l->addWidget(makeInfoKeyLabel(QStringLiteral("Server"), serverRow), 0, Qt::AlignVCenter);
            serverValueLabel = new ElidedLabel(QStringLiteral("Not configured"), Qt::ElideRight, serverRow);
            serverValueLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            l->addWidget(serverValueLabel, 1, Qt::AlignVCenter);
        }
        rowsLayout->addWidget(serverRow);

        // 2. Model
        modelRow = new QWidget(infoRowsContainer);
        {
            auto* l = new QHBoxLayout(modelRow);
            l->setContentsMargins(4, 1, 4, 1);
            l->setSpacing(5);
            l->addWidget(makeInfoKeyLabel(QStringLiteral("Model"), modelRow), 0, Qt::AlignVCenter);
            modelValueLabel = new ElidedLabel(QStringLiteral("Not configured"), Qt::ElideRight, modelRow);
            l->addWidget(modelValueLabel, 1, Qt::AlignVCenter);
        }
        rowsLayout->addWidget(modelRow);

        // 3. Message
        messageRow = new QWidget(infoRowsContainer);
        {
            auto* l = new QHBoxLayout(messageRow);
            l->setContentsMargins(4, 1, 4, 1);
            l->setSpacing(5);
            l->addWidget(makeInfoKeyLabel(QStringLiteral("Message"), messageRow), 0, Qt::AlignVCenter);
            messageValueLabel = new ElidedLabel(QStringLiteral("Idle"), Qt::ElideRight, messageRow);
            l->addWidget(messageValueLabel, 1, Qt::AlignVCenter);
        }
        rowsLayout->addWidget(messageRow);

        // 4. Optional Tags row
        tagsRow = new QWidget(infoRowsContainer);
        {
            auto* l = new QHBoxLayout(tagsRow);
            l->setContentsMargins(4, 1, 4, 1);
            l->setSpacing(5);
            l->addWidget(makeInfoKeyLabel(QStringLiteral("Tags"), tagsRow), 0, Qt::AlignVCenter);
            tagsValueLabel = new ElidedLabel(QString(), Qt::ElideRight, tagsRow);
            l->addWidget(tagsValueLabel, 1, Qt::AlignVCenter);
        }
        tagsRow->setVisible(false);
        rowsLayout->addWidget(tagsRow);

        // 7. Optional Timers row
        timersRow = new QWidget(infoRowsContainer);
        {
            auto* l = new QHBoxLayout(timersRow);
            l->setContentsMargins(4, 1, 4, 1);
            l->setSpacing(5);
            l->addWidget(makeInfoKeyLabel(QStringLiteral("Timers"), timersRow), 0, Qt::AlignVCenter);
            timersValueLabel = new ElidedLabel(QString(), Qt::ElideRight, timersRow);
            l->addWidget(timersValueLabel, 1, Qt::AlignVCenter);
        }
        timersRow->setVisible(false);
        rowsLayout->addWidget(timersRow);

        // 8. Optional Terminal approvals row
        terminalRow = new QWidget(infoRowsContainer);
        {
            auto* l = new QHBoxLayout(terminalRow);
            l->setContentsMargins(4, 1, 4, 1);
            l->setSpacing(5);
            l->addWidget(makeInfoKeyLabel(QStringLiteral("Terminal"), terminalRow), 0, Qt::AlignVCenter);
            terminalValueLabel = new ElidedLabel(QString(), Qt::ElideRight, terminalRow);
            l->addWidget(terminalValueLabel, 1, Qt::AlignVCenter);
        }
        terminalRow->setVisible(false);
        rowsLayout->addWidget(terminalRow);

        subLayout->addWidget(infoRowsContainer);

        // Status Ticker (up to 6 newest-first rows directly beneath info rows)
        statusTickerWidget = new QWidget(infoSubpanel);
        statusTickerWidget->setObjectName(QStringLiteral("spellStatusTicker"));
        auto* tickerLayout = new QVBoxLayout(statusTickerWidget);
        tickerLayout->setContentsMargins(14, 0, 14, 10);
        tickerLayout->setSpacing(3);

        for (int i = 0; i < kTickerMaxRows; ++i) {
            auto* row = new QWidget(statusTickerWidget);
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(6);

            auto* timeLbl = new QLabel(row);
            timeLbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            timeLbl->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

            auto* textLbl = new ElidedLabel(QString(), Qt::ElideRight, row);
            rowLayout->addWidget(timeLbl, 0, Qt::AlignVCenter);
            rowLayout->addWidget(textLbl, 1, Qt::AlignVCenter);

            row->setVisible(false);
            tickerLayout->addWidget(row);
            tickerRows[static_cast<std::size_t>(i)] = TickerRowUi{row, timeLbl, textLbl};
        }

        statusTickerWidget->setVisible(false);
        subLayout->addWidget(statusTickerWidget);
    }

    void buildProjectsSubpanel() {
        projectsSubpanel = new QWidget(owner);
        projectsSubpanel->setObjectName(QStringLiteral("spellProjectsSubpanel"));
        projectsSubpanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        auto* subLayout = new QVBoxLayout(projectsSubpanel);
        subLayout->setContentsMargins(0, 0, 0, 0);
        subLayout->setSpacing(0);

        // Header row: padding 6px 8px 6px 14px, "PROJECTS" (10pt bold mono textMuted) + spacer + "+"
        auto* headerRow = new QWidget(projectsSubpanel);
        auto* headerLayout = new QHBoxLayout(headerRow);
        headerLayout->setContentsMargins(14, 6, 8, 6);
        headerLayout->setSpacing(4);

        projectsSectionTitle = new QLabel(QStringLiteral("PROJECTS"), headerRow);
        projectsSectionTitle->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        headerLayout->addWidget(projectsSectionTitle, 0, Qt::AlignVCenter);
        headerLayout->addStretch(1);

        addProjectBtn = new SpellIconButton(
            QStringLiteral("plus"),
            QStringLiteral("Add Project Folder"),
            BadgeTone::Neutral,
            headerRow);
        addProjectBtn->setIconPixelSize(12);
        addProjectBtn->setFixedSize(22, 22);
        addProjectBtn->setFocusPolicy(Qt::NoFocus);
        QObject::connect(addProjectBtn, &QToolButton::clicked, owner, [this]() {
            emit owner->addProjectRequested();
        });
        headerLayout->addWidget(addProjectBtn, 0, Qt::AlignVCenter);
        subLayout->addWidget(headerRow);

        // Stack: empty state vs scrollable expandable project sections
        projectsStack = new QStackedWidget(projectsSubpanel);

        // Page 0: Clean empty surface (project folders are added via the '+' button on the PROJECTS header)
        projectsEmptyPage = new QWidget(projectsStack);
        projectsEmptyPage->setObjectName(QStringLiteral("spellProjectsEmptyPage"));
        projectsStack->addWidget(projectsEmptyPage);

        // Page 1: QScrollArea filling remaining vertical space
        projectsScrollArea = new QScrollArea(projectsStack);
        projectsScrollArea->setObjectName(QStringLiteral("spellProjectsScrollArea"));
        projectsScrollArea->setWidgetResizable(true);
        projectsScrollArea->setFrameShape(QFrame::NoFrame);
        projectsScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        projectsScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

        projectsScrollContent = new QWidget(projectsScrollArea);
        projectsScrollContent->setObjectName(QStringLiteral("spellProjectsScrollContent"));
        projectsListLayout = new QVBoxLayout(projectsScrollContent);
        projectsListLayout->setContentsMargins(8, 0, 8, 8);
        projectsListLayout->setSpacing(2);
        projectsListLayout->addStretch(1);

        projectsScrollArea->setWidget(projectsScrollContent);
        projectsStack->addWidget(projectsScrollArea);

        subLayout->addWidget(projectsStack, 1);

        // F2 keyboard shortcut renames active conversation when explorer has focus
        auto* renameShortcut = new QShortcut(QKeySequence(Qt::Key_F2), projectsSubpanel);
        renameShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        QObject::connect(renameShortcut, &QShortcut::activated, owner, [this]() {
            if (const ThreadSummary* active = findThread(activeThreadId)) {
                const QString title = active->title.trimmed().isEmpty()
                    ? QStringLiteral("Untitled Conversation")
                    : active->title.trimmed();
                promptRenameThread(active->id, title);
            }
        });
    }

    void buildSettingsSubpanel() {
        settingsSubpanel = new QWidget(owner);
        settingsSubpanel->setObjectName(QStringLiteral("spellSettingsSubpanel"));
        settingsSubpanel->setFixedHeight(48);

        auto* footerLayout = new QHBoxLayout(settingsSubpanel);
        footerLayout->setContentsMargins(14, 0, 14, 0);
        footerLayout->setSpacing(0);

        footerHatLabel = new QLabel(settingsSubpanel);
        footerHatLabel->setFixedSize(22, 22);
        footerHatLabel->setAlignment(Qt::AlignCenter);
        footerHatLabel->setToolTip(QStringLiteral("BeatrixLeslie IDE"));
        footerLayout->addWidget(footerHatLabel, 0, Qt::AlignVCenter);

        footerLayout->addStretch(1);

        settingsGearBtn = new SpellIconButton(
            QStringLiteral("gear"),
            settingsShortcutHint(),
            BadgeTone::Accent,
            settingsSubpanel);
        settingsGearBtn->setObjectName(QStringLiteral("spellSettingsGearBtn"));
        settingsGearBtn->setIconPixelSize(16);
        settingsGearBtn->setFixedSize(30, 30);
        settingsGearBtn->setFocusPolicy(Qt::NoFocus);
        QObject::connect(settingsGearBtn, &QToolButton::clicked, owner, [this]() {
            emit owner->settingsRequested(static_cast<int>(SettingsPaneId::General));
        });
        footerLayout->addWidget(settingsGearBtn, 0, Qt::AlignVCenter);
    }

    // -------------------------------------------------------------------------
    // Lookup & linked folder resolution
    // -------------------------------------------------------------------------

    const ProjectInfo* findProject(const QString& projectId) const {
        for (const ProjectInfo& p : projects) {
            if (p.id == projectId) {
                return &p;
            }
        }
        return nullptr;
    }

    const ThreadSummary* findThread(const QString& threadId) const {
        for (const ThreadSummary& t : allThreads) {
            if (t.id == threadId) {
                return &t;
            }
        }
        return nullptr;
    }

    QString defaultProjectRootForThread(const ThreadSummary& thread) const {
        if (!thread.projectId.isEmpty()) {
            if (const ProjectInfo* proj = findProject(thread.projectId)) {
                const QString trimmed = proj->rootPath.trimmed();
                if (!trimmed.isEmpty()) {
                    return QDir::cleanPath(trimmed);
                }
            }
        }
        if (const ProjectInfo* activeProj = findProject(activeProjectId)) {
            const QString trimmed = activeProj->rootPath.trimmed();
            if (!trimmed.isEmpty()) {
                return QDir::cleanPath(trimmed);
            }
        }
        return {};
    }

    QString effectiveLinkedFolderForThread(const ThreadSummary& thread,
                                           const QString& fallbackRoot) const {
        // 1. Local override map if non-empty and exists as a directory
        const QString overridePath = linkedFolders.value(thread.id).trimmed();
        if (!overridePath.isEmpty()) {
            const QFileInfo fi(overridePath);
            if (fi.exists() && fi.isDir()) {
                return QDir::cleanPath(overridePath);
            }
        }
        // 2. ThreadSummary::linkedFolder (or raw snapshot fallback) if non-empty
        QString summaryLinked = extractThreadLinkedFolder(thread).trimmed();
        if (summaryLinked.isEmpty()) {
            summaryLinked = snapshotThreadLinkedFolders.value(thread.id).trimmed();
        }
        if (!summaryLinked.isEmpty()) {
            return QDir::cleanPath(summaryLinked);
        }
        // 3. Owning project rootPath
        if (!fallbackRoot.trimmed().isEmpty()) {
            return QDir::cleanPath(fallbackRoot.trimmed());
        }
        return defaultProjectRootForThread(thread);
    }

    bool isThreadFolderOverridden(const ThreadSummary& thread, const QString& projectRoot) const {
        if (!linkedFolders.value(thread.id).trimmed().isEmpty()) {
            return true;
        }
        const QString effective = effectiveLinkedFolderForThread(thread, projectRoot);
        if (!effective.isEmpty() && (projectRoot.isEmpty() || !pathsEqual(effective, projectRoot))) {
            return true;
        }
        return false;
    }

    // -------------------------------------------------------------------------
    // Info subpanel & status ticker rendering
    // -------------------------------------------------------------------------

    void updateInfoSubpanelVisuals() {
        const UiPalette u = ThemeManager::instance().ui();

        // 1. Server row
        const QString host = formatEndpointHost(endpointBaseUrl);
        const QString trimmedErr = singleLine(engineError);
        QString serverText;
        QColor serverColor = u.textSecondary;
        QFont::Weight serverWeight = QFont::Normal;
        if (!endpointConfigured) {
            serverText = QStringLiteral("Not configured");
            serverColor = u.textMuted;
        } else if (engineOnline) {
            serverText = host.isEmpty()
                ? QStringLiteral("Online")
                : QStringLiteral("Online · %1").arg(host);
            serverColor = u.textPrimary;
            serverWeight = QFont::DemiBold;
        } else if (!trimmedErr.isEmpty()) {
            serverText = QStringLiteral("Offline · %1").arg(trimmedErr);
            serverColor = u.textSecondary;
        } else {
            serverText = host.isEmpty()
                ? QStringLiteral("Offline")
                : QStringLiteral("Offline · %1").arg(host);
            serverColor = u.textSecondary;
        }
        serverValueLabel->setFont(makeMonoFont(10.5, serverWeight));
        serverValueLabel->setText(serverText);
        setElidedColor(serverValueLabel, serverColor);
        const QString serverTip = endpointConfigured && !endpointBaseUrl.trimmed().isEmpty()
            ? QStringLiteral("%1\n%2\nClick to open Engine settings").arg(serverText, endpointBaseUrl.trimmed())
            : QStringLiteral("%1\nClick to open Engine settings").arg(serverText);
        serverRow->setToolTip(serverTip);

        // 2. Model row
        const QString trimmedModel = modelId.trimmed();
        if (!trimmedModel.isEmpty()) {
            modelValueLabel->setFont(makeMonoFont(10.5, QFont::Normal));
            modelValueLabel->setText(trimmedModel);
            setElidedColor(modelValueLabel, engineOnline ? u.textPrimary : u.textSecondary);
            modelValueLabel->setToolTip(trimmedModel);
        } else {
            modelValueLabel->setFont(makeMonoFont(10.5, QFont::Normal));
            modelValueLabel->setText(QStringLiteral("Not configured"));
            setElidedColor(modelValueLabel, u.textMuted);
            modelValueLabel->setToolTip(QStringLiteral("No model configured"));
        }

        // 3. Message row
        QString msgText;
        if (isAgentBusy) {
            if (decodeTokS > 0.0) {
                msgText = QStringLiteral("Generating · %1 tok/s").arg(decodeTokS, 0, 'f', 1);
            } else {
                msgText = QStringLiteral("Generating…");
            }
        } else if (activePromptTokens > 0) {
            msgText = QStringLiteral("Idle · %1 context tokens").arg(activePromptTokens);
        } else {
            msgText = QStringLiteral("Idle");
        }
        messageValueLabel->setFont(makeMonoFont(10.5, isAgentBusy ? QFont::DemiBold : QFont::Normal));
        messageValueLabel->setText(msgText);
        setElidedColor(messageValueLabel, isAgentBusy ? u.textPrimary : u.textSecondary);
        messageValueLabel->setToolTip(msgText);

        // 4. Optional Tags row
        const bool hasTags = !activeTagsText.isEmpty();
        tagsRow->setVisible(hasTags);
        if (hasTags) {
            tagsValueLabel->setFont(makeMonoFont(10.5, QFont::Normal));
            tagsValueLabel->setText(activeTagsText);
            setElidedColor(tagsValueLabel, u.textSecondary);
            tagsValueLabel->setToolTip(activeTagsText);
        }

        // 7. Optional Timers row
        const bool hasTimers = !armedTimersText.isEmpty();
        timersRow->setVisible(hasTimers);
        if (hasTimers) {
            timersValueLabel->setFont(makeMonoFont(10.5, QFont::Normal));
            timersValueLabel->setText(armedTimersText);
            setElidedColor(timersValueLabel, u.textSecondary);
            timersValueLabel->setToolTip(armedTimersText);
        }

        // 8. Optional Terminal approvals row
        const bool hasApprovals = pendingTerminalApprovalsCount > 0;
        terminalRow->setVisible(hasApprovals);
        if (hasApprovals) {
            const QString appText = QStringLiteral("%1 approval(s) waiting").arg(pendingTerminalApprovalsCount);
            terminalValueLabel->setFont(makeMonoFont(10.5, QFont::DemiBold));
            terminalValueLabel->setText(appText);
            setElidedColor(terminalValueLabel, u.textPrimary);
            terminalValueLabel->setToolTip(appText);
        }
    }

    void refreshStatusTicker(bool forceRedraw) {
        const QVector<TickerEntry> nextEntries = buildTickerEntries(activeEvents, pushedNotices);
        if (!forceRedraw && nextEntries == renderedTickerEntries) {
            return;
        }
        renderedTickerEntries = nextEntries;

        const UiPalette u = ThemeManager::instance().ui();
        const QFont timeFont = makeMonoFont(9.5, QFont::Normal);
        const QFont textFont = makeMonoFont(10.0, QFont::Normal);

        const bool hasAny = !renderedTickerEntries.isEmpty();
        statusTickerWidget->setVisible(hasAny);

        for (int i = 0; i < kTickerMaxRows; ++i) {
            TickerRowUi& rowUi = tickerRows[static_cast<std::size_t>(i)];
            if (i < renderedTickerEntries.size()) {
                const TickerEntry& entry = renderedTickerEntries.at(i);
                const double opacity = kTickerOpacities[static_cast<std::size_t>(i)];

                const QColor timeColor = withAlpha(u.textMuted, u.textMuted.alphaF() * opacity);
                const QColor baseTextColor = entry.isError ? u.diffRemoved : u.textSecondary;
                const QColor textColor = withAlpha(baseTextColor, baseTextColor.alphaF() * opacity);

                rowUi.timeLabel->setFont(timeFont);
                rowUi.timeLabel->setText(formatTickerTime(entry.timeSecs));
                setLabelColor(rowUi.timeLabel, timeColor);

                rowUi.textLabel->setFont(textFont);
                rowUi.textLabel->setText(entry.text);
                rowUi.textLabel->setToolTip(entry.text);
                setElidedColor(rowUi.textLabel, textColor);

                rowUi.row->setVisible(true);
            } else {
                rowUi.row->setVisible(false);
            }
        }
    }

    // -------------------------------------------------------------------------
    // Nested Projects & Conversations UI rendering
    // -------------------------------------------------------------------------

    void rebuildProjectsContent() {
        const int prevScroll = projectsScrollArea && projectsScrollArea->verticalScrollBar()
            ? projectsScrollArea->verticalScrollBar()->value()
            : 0;

        projectSectionUis.clear();
        clearLayout(projectsListLayout);

        const bool hasProjects = !projects.isEmpty();
        projectsStack->setCurrentIndex(hasProjects ? 1 : 0);
        if (!hasProjects) {
            projectsListLayout->addStretch(1);
            return;
        }

        const UiPalette u = ThemeManager::instance().ui();
        const QColor solidElevated = compositeOver(u.elevatedSurface, u.background);
        const QColor solidSelected = compositeOver(u.selectedFill, u.background);

        projectSectionUis.reserve(projects.size());

        for (const ProjectInfo& project : projects) {
            const QString projectId = project.id;
            const QString rootPath = project.rootPath.trimmed();
            const QString title = projectDisplayTitle(project);
            const QString popLabel = project.name.trimmed().isEmpty() ? title : project.name;
            const bool isOpen = expandedProjectIds.contains(projectId);
            const bool isSelected = (projectId == activeProjectId);

            auto* sectionWidget = new QWidget(projectsScrollContent);
            auto* sectionLayout = new QVBoxLayout(sectionWidget);
            sectionLayout->setContentsMargins(0, 0, 0, 0);
            sectionLayout->setSpacing(1);

            ProjectSectionUi sectionUi;
            sectionUi.projectId = projectId;

            // Project header row
            auto* headerRow = new InteractiveRowWidget(sectionWidget);
            headerRow->setColors(solidElevated, solidSelected);
            headerRow->setCornerRadius(5.0);
            headerRow->setSelected(isSelected && !isOpen);
            QString headerTip = rootPath.isEmpty() ? title : rootPath;
            if (project.isReadOnly) {
                headerTip += QStringLiteral(" (Read-only)");
            }
            headerRow->setToolTip(headerTip);

            auto* headerLayout = new QHBoxLayout(headerRow);
            headerLayout->setContentsMargins(6, 4, 4, 4);
            headerLayout->setSpacing(5);

            // [chevron-down / chevron-right] toggle button (12px)
            auto* chevronBtn = new SpellIconButton(
                isOpen ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right"),
                isOpen ? QStringLiteral("Collapse project") : QStringLiteral("Expand project"),
                BadgeTone::Neutral,
                headerRow);
            chevronBtn->setIconPixelSize(12);
            chevronBtn->setFixedSize(18, 18);
            chevronBtn->setFocusPolicy(Qt::NoFocus);
            QObject::connect(chevronBtn, &QToolButton::clicked, owner, [this, projectId]() {
                if (expandedProjectIds.contains(projectId)) {
                    expandedProjectIds.remove(projectId);
                } else {
                    expandedProjectIds.insert(projectId);
                }
                syncSelectionAndExpansionInPlace();
            });
            headerLayout->addWidget(chevronBtn, 0, Qt::AlignVCenter);

            // Project Name (12.5pt, bold when selected, elided right)
            auto* nameLabel = new ElidedLabel(title, Qt::ElideRight, headerRow);
            nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            nameLabel->setFont(makeUiFont(12.5, isSelected ? QFont::DemiBold : QFont::Normal));
            setElidedColor(nameLabel, u.textPrimary);
            headerLayout->addWidget(nameLabel, 1, Qt::AlignVCenter);

            // Message Terminal Pop-Out button
            const QString projectPopTip = rootPath.isEmpty()
                ? QStringLiteral("Pop out Message Terminal")
                : QStringLiteral("Pop out Message Terminal in %1").arg(rootPath);
            auto* projectPopOutBtn = new SpellIconButton(
                QStringLiteral("terminal-popout"),
                projectPopTip,
                BadgeTone::Neutral,
                headerRow);
            projectPopOutBtn->setObjectName(QStringLiteral("spellProjectTerminalPopOutBtn"));
            projectPopOutBtn->setIconPixelSize(13);
            projectPopOutBtn->setFixedSize(22, 22);
            projectPopOutBtn->setFocusPolicy(Qt::NoFocus);
            projectPopOutBtn->setEnabled(!rootPath.isEmpty());
            QObject::connect(projectPopOutBtn, &QToolButton::clicked, owner, [this, rootPath, popLabel]() {
                if (!rootPath.isEmpty()) {
                    emit owner->terminalPopOutRequested(rootPath, popLabel);
                }
            });
            headerLayout->addWidget(projectPopOutBtn, 0, Qt::AlignVCenter);

            // Optional subtle "lock" icon if project.isReadOnly
            QLabel* readOnlyIcon = nullptr;
            if (project.isReadOnly) {
                readOnlyIcon = new QLabel(headerRow);
                readOnlyIcon->setFixedSize(14, 14);
                readOnlyIcon->setAlignment(Qt::AlignCenter);
                readOnlyIcon->setToolTip(QStringLiteral("Read-only"));
                readOnlyIcon->setPixmap(
                    icons::vectorIcon(QStringLiteral("lock"), u.textMuted, 11).pixmap(11, 11));
                headerLayout->addWidget(readOnlyIcon, 0, Qt::AlignVCenter);
            }

            // Click expands & selects project; double-click triggers rename dialog
            headerRow->onClick = [this, projectId]() {
                expandedProjectIds.insert(projectId);
                const bool changed = (activeProjectId != projectId);
                activeProjectId = projectId;
                syncSelectionAndExpansionInPlace();
                if (changed) {
                    emit owner->selectProjectRequested(projectId);
                }
            };
            headerRow->onDoubleClick = [this, projectId, title]() {
                promptRenameProject(projectId, title);
            };
            headerRow->onContextMenu = [this, projectId](const QPoint& globalPos) {
                showProjectContextMenu(projectId, globalPos);
            };

            sectionLayout->addWidget(headerRow);

            // Nested Conversation list (indented 18px left directly under expanded project)
            auto* convContainer = new QWidget(sectionWidget);
            auto* convLayout = new QVBoxLayout(convContainer);
            convLayout->setContentsMargins(18, 1, 0, 2);
            convLayout->setSpacing(1);

            // 1. Subtle "+ New Conversation" row
            auto* newConvRow = new InteractiveRowWidget(convContainer);
            newConvRow->setColors(solidElevated, solidSelected);
            newConvRow->setCornerRadius(5.0);
            newConvRow->setToolTip(QStringLiteral("Start a new conversation in %1").arg(title));
            auto* newConvLayout = new QHBoxLayout(newConvRow);
            newConvLayout->setContentsMargins(6, 4, 6, 4);
            newConvLayout->setSpacing(5);

            auto* newConvIcon = new QLabel(newConvRow);
            newConvIcon->setFixedSize(12, 12);
            newConvIcon->setAlignment(Qt::AlignCenter);
            newConvIcon->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            newConvIcon->setPixmap(icons::vectorIcon(QStringLiteral("plus"), u.textMuted, 10).pixmap(10, 10));
            newConvLayout->addWidget(newConvIcon, 0, Qt::AlignVCenter);

            auto* newConvLabel = new QLabel(QStringLiteral("New Conversation"), newConvRow);
            newConvLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            newConvLabel->setFont(makeUiFont(11.5, QFont::Normal));
            setLabelColor(newConvLabel, u.textMuted);
            newConvLayout->addWidget(newConvLabel, 1, Qt::AlignVCenter);

            newConvRow->onClick = [this, projectId]() {
                expandedProjectIds.insert(projectId);
                emit owner->createThreadRequested(projectId);
            };
            convLayout->addWidget(newConvRow);

            // 2. Conversations for this project (sorted newest first)
            QVector<ThreadSummary> projectThreads;
            for (const ThreadSummary& thread : allThreads) {
                if (thread.projectId == projectId ||
                    (thread.projectId.isEmpty() && projectId == activeProjectId)) {
                    projectThreads.append(thread);
                }
            }
            std::stable_sort(projectThreads.begin(),
                             projectThreads.end(),
                             [](const ThreadSummary& a, const ThreadSummary& b) {
                                 return a.updatedAt > b.updatedAt;
                             });

            sectionUi.threadRows.reserve(projectThreads.size());

            for (const ThreadSummary& thread : projectThreads) {
                const QString threadId = thread.id;
                const bool isActiveThread = (threadId == activeThreadId);
                const QString displayTitle = thread.title.trimmed().isEmpty()
                    ? QStringLiteral("Untitled Conversation")
                    : thread.title.trimmed();
                const QString threadPopLabel = thread.title.trimmed().isEmpty()
                    ? displayTitle
                    : thread.title;
                const QString effectiveFolder = effectiveLinkedFolderForThread(thread, rootPath);
                const bool hasCustomFolder =
                    !effectiveFolder.isEmpty() && !rootPath.isEmpty() && !pathsEqual(effectiveFolder, rootPath);
                QString customFolderBasename;
                if (hasCustomFolder) {
                    customFolderBasename = QFileInfo(effectiveFolder).fileName();
                    if (customFolderBasename.isEmpty()) {
                        customFolderBasename = compactPathForDisplay(effectiveFolder);
                    }
                }

                auto* threadRow = new InteractiveRowWidget(convContainer);
                threadRow->setColors(solidElevated, solidSelected);
                threadRow->setCornerRadius(5.0);
                threadRow->setSelected(isActiveThread);

                QStringList tipLines;
                tipLines.append(displayTitle);
                tipLines.append(thread.profile == QLatin1StringView("ask")
                                    ? QStringLiteral("Mode: Ask (read-only)")
                                    : QStringLiteral("Mode: Write"));
                if (!effectiveFolder.isEmpty()) {
                    tipLines.append(QStringLiteral("Folder: %1").arg(effectiveFolder));
                }
                const QString updatedStr = formatRelativeTimestamp(thread.updatedAt);
                if (!updatedStr.isEmpty()) {
                    tipLines.append(QStringLiteral("Updated %1").arg(updatedStr));
                }
                threadRow->setToolTip(tipLines.join(QLatin1Char('\n')));

                auto* rowLayout = new QHBoxLayout(threadRow);
                rowLayout->setContentsMargins(6, 4, 4, 4);
                rowLayout->setSpacing(5);

                auto* titleLabel = new ElidedLabel(displayTitle, Qt::ElideRight, threadRow);
                titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
                titleLabel->setFont(makeUiFont(12.0, isActiveThread ? QFont::DemiBold : QFont::Normal));
                setElidedColor(titleLabel, isActiveThread ? u.textPrimary : u.textSecondary);

                QLabel* folderIconLbl = nullptr;
                ElidedLabel* folderBaseLbl = nullptr;
                if (hasCustomFolder) {
                    auto* textCol = new QVBoxLayout();
                    textCol->setContentsMargins(0, 0, 0, 0);
                    textCol->setSpacing(1);
                    textCol->addWidget(titleLabel);

                    auto* folderSubRow = new QHBoxLayout();
                    folderSubRow->setContentsMargins(0, 0, 0, 0);
                    folderSubRow->setSpacing(4);

                    folderIconLbl = new QLabel(threadRow);
                    folderIconLbl->setFixedSize(11, 11);
                    folderIconLbl->setAlignment(Qt::AlignCenter);
                    folderIconLbl->setAttribute(Qt::WA_TransparentForMouseEvents, true);
                    folderIconLbl->setPixmap(
                        icons::vectorIcon(QStringLiteral("folder"), u.textMuted, 10).pixmap(10, 10));
                    folderSubRow->addWidget(folderIconLbl, 0, Qt::AlignVCenter);

                    folderBaseLbl = new ElidedLabel(customFolderBasename, Qt::ElideRight, threadRow);
                    folderBaseLbl->setAttribute(Qt::WA_TransparentForMouseEvents, true);
                    folderBaseLbl->setFont(makeMonoFont(9.5, QFont::Normal));
                    setElidedColor(folderBaseLbl, u.textMuted);
                    folderSubRow->addWidget(folderBaseLbl, 1, Qt::AlignVCenter);

                    textCol->addLayout(folderSubRow);
                    rowLayout->addLayout(textCol, 1);
                } else {
                    rowLayout->addWidget(titleLabel, 1, Qt::AlignVCenter);
                }

                // Message Terminal Pop-Out button
                const QString threadPopTip = effectiveFolder.isEmpty()
                    ? QStringLiteral("Pop out Message Terminal")
                    : QStringLiteral("Pop out Message Terminal in %1").arg(effectiveFolder);
                auto* threadPopOutBtn = new SpellIconButton(
                    QStringLiteral("terminal-popout"),
                    threadPopTip,
                    BadgeTone::Neutral,
                    threadRow);
                threadPopOutBtn->setObjectName(QStringLiteral("spellThreadTerminalPopOutBtn"));
                threadPopOutBtn->setIconPixelSize(13);
                threadPopOutBtn->setFixedSize(22, 22);
                threadPopOutBtn->setFocusPolicy(Qt::NoFocus);
                threadPopOutBtn->setEnabled(!effectiveFolder.isEmpty());
                QObject::connect(threadPopOutBtn,
                                 &QToolButton::clicked,
                                 owner,
                                 [this, effectiveFolder, threadPopLabel]() {
                                     if (!effectiveFolder.isEmpty()) {
                                         emit owner->terminalPopOutRequested(effectiveFolder, threadPopLabel);
                                     }
                                 });
                rowLayout->addWidget(threadPopOutBtn, 0, Qt::AlignVCenter);

                threadRow->onClick = [this, threadId, projectId]() {
                    activeThreadId = threadId;
                    if (!projectId.isEmpty()) {
                        activeProjectId = projectId;
                    }
                    syncSelectionAndExpansionInPlace();
                    emit owner->selectThreadRequested(threadId);
                };
                threadRow->onDoubleClick = [this, threadId, displayTitle]() {
                    promptRenameThread(threadId, displayTitle);
                };
                threadRow->onContextMenu = [this, threadId, projectId](const QPoint& globalPos) {
                    showThreadContextMenu(threadId, projectId, globalPos);
                };

                convLayout->addWidget(threadRow);
                sectionUi.threadRows.append(ThreadRowUi{
                    threadId, threadRow, titleLabel, folderBaseLbl, folderIconLbl
                });
            }

            convContainer->setVisible(isOpen);
            sectionLayout->addWidget(convContainer);

            sectionUi.headerRow = headerRow;
            sectionUi.chevronBtn = chevronBtn;
            sectionUi.nameLabel = nameLabel;
            sectionUi.readOnlyIconLabel = readOnlyIcon;
            sectionUi.conversationsContainer = convContainer;
            sectionUi.newConversationRow = newConvRow;
            sectionUi.newConversationIcon = newConvIcon;
            sectionUi.newConversationLabel = newConvLabel;
            projectSectionUis.append(std::move(sectionUi));

            projectsListLayout->addWidget(sectionWidget);
        }

        projectsListLayout->addStretch(1);

        if (projectsScrollArea && projectsScrollArea->verticalScrollBar()) {
            projectsScrollArea->verticalScrollBar()->setValue(prevScroll);
        }
    }

    void syncSelectionAndExpansionInPlace() {
        const UiPalette u = ThemeManager::instance().ui();
        for (ProjectSectionUi& section : projectSectionUis) {
            const bool isOpen = expandedProjectIds.contains(section.projectId);
            const bool isSelected = (section.projectId == activeProjectId);

            if (section.headerRow) {
                section.headerRow->setSelected(isSelected && !isOpen);
            }
            if (section.chevronBtn) {
                section.chevronBtn->setIconName(
                    isOpen ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right"));
                section.chevronBtn->setToolTip(
                    isOpen ? QStringLiteral("Collapse project") : QStringLiteral("Expand project"));
            }
            if (section.nameLabel) {
                section.nameLabel->setFont(makeUiFont(12.5, isSelected ? QFont::DemiBold : QFont::Normal));
                setElidedColor(section.nameLabel, u.textPrimary);
            }
            if (section.conversationsContainer) {
                section.conversationsContainer->setVisible(isOpen);
            }
            for (ThreadRowUi& threadUi : section.threadRows) {
                const bool isActive = (threadUi.threadId == activeThreadId);
                if (threadUi.row) {
                    threadUi.row->setSelected(isActive);
                }
                if (threadUi.titleLabel) {
                    threadUi.titleLabel->setFont(makeUiFont(12.0, isActive ? QFont::DemiBold : QFont::Normal));
                    setElidedColor(threadUi.titleLabel, isActive ? u.textPrimary : u.textSecondary);
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Dialogs & Context Menus
    // -------------------------------------------------------------------------

    void promptRenameProject(const QString& projectId, const QString& currentName) {
        if (projectId.isEmpty()) {
            return;
        }
        bool accepted = false;
        const QString updated = QInputDialog::getText(
            owner,
            QStringLiteral("Rename Project"),
            QStringLiteral("Project name:"),
            QLineEdit::Normal,
            currentName,
            &accepted);
        if (!accepted) {
            return;
        }
        const QString trimmed = updated.trimmed();
        if (!trimmed.isEmpty() && trimmed != currentName) {
            emit owner->renameProjectRequested(projectId, trimmed);
        }
    }

    void promptRenameThread(const QString& threadId, const QString& currentTitle) {
        if (threadId.isEmpty()) {
            return;
        }
        bool accepted = false;
        const QString updated = QInputDialog::getText(
            owner,
            QStringLiteral("Rename Conversation"),
            QStringLiteral("Conversation title:"),
            QLineEdit::Normal,
            currentTitle,
            &accepted);
        if (!accepted) {
            return;
        }
        const QString trimmed = updated.trimmed();
        if (!trimmed.isEmpty() && trimmed != currentTitle) {
            emit owner->renameThreadRequested(threadId, trimmed);
        }
    }

    void promptChangeLinkedFolder(const QString& threadId,
                                  const QString& currentFolder,
                                  const QString& projectRoot) {
        if (threadId.isEmpty()) {
            return;
        }
        QString startDir;
        if (!currentFolder.isEmpty() && QFileInfo::exists(currentFolder)) {
            startDir = currentFolder;
        } else if (!projectRoot.isEmpty() && QFileInfo::exists(projectRoot)) {
            startDir = projectRoot;
        } else {
            startDir = QDir::homePath();
        }
        const QString chosenDir = QFileDialog::getExistingDirectory(
            owner,
            QStringLiteral("Select Linked Folder for Conversation"),
            startDir,
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (chosenDir.trimmed().isEmpty()) {
            return;
        }
        const QString cleaned = QDir::cleanPath(chosenDir.trimmed());
        linkedFolders.insert(threadId, cleaned);
        rebuildProjectsContent();
        emit owner->conversationLinkedFolderChanged(threadId, cleaned);
    }

    void resetLinkedFolder(const QString& threadId) {
        if (threadId.isEmpty()) {
            return;
        }
        linkedFolders.remove(threadId);
        snapshotThreadLinkedFolders.remove(threadId);
        rebuildProjectsContent();
        emit owner->conversationLinkedFolderChanged(threadId, QString());
    }

    void showProjectContextMenu(const QString& projectId, const QPoint& globalPos) {
        const ProjectInfo* proj = findProject(projectId);
        if (!proj) {
            return;
        }

        const UiPalette u = ThemeManager::instance().ui();
        const QString rootPath = proj->rootPath.trimmed();
        const QString title = projectDisplayTitle(*proj);
        const QString popLabel = proj->name.trimmed().isEmpty() ? title : proj->name;
        const bool isReadOnly = proj->isReadOnly;

        QMenu menu(owner);

        QAction* newChatAct = menu.addAction(
            icons::vectorIcon(QStringLiteral("plus"), u.textPrimary, 14),
            QStringLiteral("New Conversation"));
        QAction* popOutAct = menu.addAction(
            icons::vectorIcon(QStringLiteral("terminal-popout"), u.textPrimary, 14),
            QStringLiteral("Pop Out Terminal Here"));
        popOutAct->setEnabled(!rootPath.isEmpty());
        QAction* openFileAct = menu.addAction(
            icons::vectorIcon(QStringLiteral("folder-open"), u.textPrimary, 14),
            QStringLiteral("Open File in Project…"));
        openFileAct->setEnabled(!rootPath.isEmpty());

        menu.addSeparator();

        QAction* renameAct = menu.addAction(
            icons::vectorIcon(QStringLiteral("edit"), u.textPrimary, 14),
            QStringLiteral("Rename…"));
        QAction* toggleRoAct = menu.addAction(
            icons::vectorIcon(isReadOnly ? QStringLiteral("unlock") : QStringLiteral("lock"), u.textPrimary, 14),
            isReadOnly ? QStringLiteral("Allow Edits") : QStringLiteral("Make Read-Only"));

        menu.addSeparator();

        QAction* revealAct = menu.addAction(
            icons::vectorIcon(QStringLiteral("external-link"), u.textSecondary, 14),
            QStringLiteral("Reveal in File Manager"));
        revealAct->setEnabled(!rootPath.isEmpty() && QFileInfo::exists(rootPath));

        menu.addSeparator();

        QAction* removeAct = menu.addAction(
            icons::vectorIcon(QStringLiteral("trash"), u.danger, 14),
            QStringLiteral("Remove Project"));

        QObject::connect(newChatAct, &QAction::triggered, owner, [this, projectId]() {
            expandedProjectIds.insert(projectId);
            emit owner->createThreadRequested(projectId);
        });
        QObject::connect(popOutAct, &QAction::triggered, owner, [this, rootPath, popLabel]() {
            if (!rootPath.isEmpty()) {
                emit owner->terminalPopOutRequested(rootPath, popLabel);
            }
        });
        QObject::connect(openFileAct, &QAction::triggered, owner, [this, rootPath]() {
            if (!rootPath.isEmpty()) {
                emit owner->openProjectFileRequested(rootPath);
            }
        });
        QObject::connect(renameAct, &QAction::triggered, owner, [this, projectId, title]() {
            promptRenameProject(projectId, title);
        });
        QObject::connect(toggleRoAct, &QAction::triggered, owner, [this, projectId, isReadOnly]() {
            emit owner->toggleProjectReadOnlyRequested(projectId, !isReadOnly);
        });
        QObject::connect(revealAct, &QAction::triggered, owner, [rootPath]() {
            if (!rootPath.isEmpty()) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(rootPath));
            }
        });
        QObject::connect(removeAct, &QAction::triggered, owner, [this, projectId]() {
            emit owner->removeProjectRequested(projectId);
        });

        menu.exec(globalPos);
    }

    void showThreadContextMenu(const QString& threadId,
                               const QString& projectId,
                               const QPoint& globalPos) {
        const ThreadSummary* thread = findThread(threadId);
        if (!thread) {
            return;
        }

        const UiPalette u = ThemeManager::instance().ui();
        const QString displayTitle = thread->title.trimmed().isEmpty()
            ? QStringLiteral("Untitled Conversation")
            : thread->title.trimmed();
        const QString popLabel = thread->title.trimmed().isEmpty() ? displayTitle : thread->title;

        QString projectRoot;
        if (const ProjectInfo* proj = findProject(projectId)) {
            projectRoot = proj->rootPath.trimmed();
        }
        if (projectRoot.isEmpty()) {
            projectRoot = defaultProjectRootForThread(*thread);
        }
        const QString effectiveFolder = effectiveLinkedFolderForThread(*thread, projectRoot);
        const bool overridden = isThreadFolderOverridden(*thread, projectRoot);

        QMenu menu(owner);

        QAction* renameAct = menu.addAction(
            icons::vectorIcon(QStringLiteral("edit"), u.textPrimary, 14),
            QStringLiteral("Rename"));
        const bool isAskMode = (thread->profile.compare(QLatin1StringView("ask"), Qt::CaseInsensitive) == 0);
        QAction* toggleProfileAct = menu.addAction(
            icons::vectorIcon(isAskMode ? QStringLiteral("edit") : QStringLiteral("lock"), u.textPrimary, 14),
            isAskMode ? QStringLiteral("Switch to Write") : QStringLiteral("Switch to Ask (read-only)"));
        QAction* popOutAct = menu.addAction(
            icons::vectorIcon(QStringLiteral("terminal-popout"), u.textPrimary, 14),
            QStringLiteral("Pop Out Terminal Here"));
        popOutAct->setEnabled(!effectiveFolder.isEmpty());
        QAction* changeFolderAct = menu.addAction(
            icons::vectorIcon(QStringLiteral("folder-open"), u.textPrimary, 14),
            QStringLiteral("Change Linked Folder…"));

        QAction* resetFolderAct = nullptr;
        if (overridden) {
            resetFolderAct = menu.addAction(
                icons::vectorIcon(QStringLiteral("refresh"), u.textSecondary, 14),
                QStringLiteral("Reset Linked Folder to Project Root"));
        }

        menu.addSeparator();

        QAction* deleteAct = menu.addAction(
            icons::vectorIcon(QStringLiteral("trash"), u.danger, 14),
            QStringLiteral("Delete Conversation"));

        QObject::connect(renameAct, &QAction::triggered, owner, [this, threadId, displayTitle]() {
            promptRenameThread(threadId, displayTitle);
        });
        QObject::connect(toggleProfileAct, &QAction::triggered, owner, [this, threadId, isAskMode]() {
            emit owner->selectThreadRequested(threadId);
            emit owner->setThreadProfileRequested(threadId,
                                                  isAskMode ? QStringLiteral("write") : QStringLiteral("ask"));
        });
        QObject::connect(popOutAct, &QAction::triggered, owner, [this, effectiveFolder, popLabel]() {
            if (!effectiveFolder.isEmpty()) {
                emit owner->terminalPopOutRequested(effectiveFolder, popLabel);
            }
        });
        QObject::connect(changeFolderAct,
                         &QAction::triggered,
                         owner,
                         [this, threadId, effectiveFolder, projectRoot]() {
                             promptChangeLinkedFolder(threadId, effectiveFolder, projectRoot);
                         });
        if (resetFolderAct) {
            QObject::connect(resetFolderAct, &QAction::triggered, owner, [this, threadId]() {
                resetLinkedFolder(threadId);
            });
        }
        QObject::connect(deleteAct, &QAction::triggered, owner, [this, threadId]() {
            emit owner->deleteThreadRequested(threadId);
        });

        menu.exec(globalPos);
    }

    // -------------------------------------------------------------------------
    // Theme application
    // -------------------------------------------------------------------------

    void applyTheme() {
        const UiPalette u = ThemeManager::instance().ui();
        const QColor solidElevated = compositeOver(u.elevatedSurface, u.background);
        const QColor solidSelected = compositeOver(u.selectedFill, u.background);

        QPalette basePal = owner->palette();
        basePal.setColor(QPalette::Window, u.background);
        basePal.setColor(QPalette::Base, u.surface);
        basePal.setColor(QPalette::WindowText, u.textPrimary);
        basePal.setColor(QPalette::Text, u.textPrimary);
        basePal.setColor(QPalette::Highlight, solidSelected);
        basePal.setColor(QPalette::HighlightedText, u.textPrimary);
        owner->setPalette(basePal);

        owner->setStyleSheet(QStringLiteral(
            "#spellExplorerRoot, #spellInfoSubpanel, #spellInfoTopBar, #spellStatusTicker, "
            "#spellProjectsSubpanel, #spellProjectsEmptyPage, #spellProjectsScrollArea, #spellProjectsScrollContent, "
            "#spellSettingsSubpanel {\n"
            "  background-color: %1;\n"
            "  color: %2;\n"
            "}\n"
            "#spellProjectsScrollArea > QWidget > QWidget {\n"
            "  background-color: %1;\n"
            "}\n")
            .arg(qssColor(u.background), qssColor(u.textPrimary)));

        // Top bar title
        panelTitleLabel->setFont(makeUiFont(12.0, QFont::DemiBold));
        setLabelColor(panelTitleLabel, u.textPrimary);

        // Info key labels (fixed 56px, textMuted 10.5pt)
        const QFont keyFont = makeUiFont(10.5, QFont::Normal);
        for (QLabel* lbl : infoKeyLabels) {
            lbl->setFont(keyFont);
            setLabelColor(lbl, u.textMuted);
        }
        serverRow->setColors(solidElevated, solidSelected);

        // Middle Projects header & empty state
        projectsSectionTitle->setFont(makeMonoFont(10.0, QFont::Bold));
        setLabelColor(projectsSectionTitle, u.textMuted);

        // Bottom Settings subpanel (22x22 Witch Hat + 30x30 gear)
        footerHatLabel->setPixmap(icons::hatMarkPixmap(22, u.textPrimary));

        // Propagate theme to custom Spell widgets
        topHairline->applyTheme();
        bottomHairline->applyTheme();
        addProjectBtn->applyTheme();
        settingsGearBtn->applyTheme();

        // Refresh themed dynamic content
        updateInfoSubpanelVisuals();
        refreshStatusTicker(/*forceRedraw=*/true);
        rebuildProjectsContent();
    }
};

// =============================================================================
// ProjectExplorerPane
// =============================================================================

ProjectExplorerPane::ProjectExplorerPane(QWidget* parent)
    : QWidget(parent)
    , m_impl(std::make_unique<Impl>()) {
    m_impl->owner = this;
    setObjectName(QStringLiteral("spellExplorerRoot"));
    setAutoFillBackground(true);
    setMinimumWidth(180);

    m_impl->buildUi();
    m_impl->applyTheme();

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &ProjectExplorerPane::applyTheme);
}

ProjectExplorerPane::~ProjectExplorerPane() = default;

void ProjectExplorerPane::setConversationLinkedFolders(const QHash<QString, QString>& linkedFolders) {
    QHash<QString, QString> normalized;
    normalized.reserve(linkedFolders.size());
    for (auto it = linkedFolders.constBegin(); it != linkedFolders.constEnd(); ++it) {
        const QString threadId = it.key().trimmed();
        const QString folderPath = it.value().trimmed();
        if (!threadId.isEmpty() && !folderPath.isEmpty()) {
            normalized.insert(threadId, QDir::cleanPath(folderPath));
        }
    }
    m_impl->linkedFolders = std::move(normalized);
    m_impl->rebuildProjectsContent();
}

void ProjectExplorerPane::pushStatusNotice(const QString& text, bool isError) {
    const QString cleaned = singleLine(text);
    if (cleaned.isEmpty()) {
        return;
    }
    const quint64 seq = ++m_impl->nextNoticeSeq;
    m_impl->pushedNotices.append(PushedNotice{
        QStringLiteral("notice-%1").arg(seq),
        QDateTime::currentSecsSinceEpoch(),
        seq,
        cleaned,
        isError
    });
    while (m_impl->pushedNotices.size() > kMaxPushedNotices) {
        m_impl->pushedNotices.removeFirst();
    }
    m_impl->refreshStatusTicker(/*forceRedraw=*/false);
}

void ProjectExplorerPane::updateSnapshot(const WorkspaceSnapshot& snap) {
    // 1. Resolve active project & expansion defaults
    QString resolvedProjectId = snap.selectedProjectId;
    const ProjectInfo* activeProj = nullptr;
    for (const ProjectInfo& p : snap.projects) {
        if (p.id == resolvedProjectId) {
            activeProj = &p;
            break;
        }
    }
    if (!activeProj && !snap.projects.isEmpty()) {
        activeProj = &snap.projects.first();
        resolvedProjectId = activeProj->id;
    }

    const bool projectsListChanged = (m_impl->projects != snap.projects);
    const bool selectedProjectChanged = (m_impl->activeProjectId != resolvedProjectId);
    const bool threadsListChanged = (m_impl->allThreads != snap.threads);
    const bool selectedThreadChanged = (m_impl->activeThreadId != snap.selectedThreadId);
    const QHash<QString, QString> nextSnapshotLinkedFolders = extractSnapshotThreadLinkedFolders(snap);
    const bool snapshotFoldersChanged = (m_impl->snapshotThreadLinkedFolders != nextSnapshotLinkedFolders);

    // Prune expandedProjectIds for removed projects and default-expand newly selected project
    if (projectsListChanged) {
        QSet<QString> validIds;
        validIds.reserve(snap.projects.size());
        for (const ProjectInfo& p : snap.projects) {
            validIds.insert(p.id);
        }
        m_impl->expandedProjectIds.intersect(validIds);
    }
    if (!resolvedProjectId.isEmpty() && (selectedProjectChanged || m_impl->expandedProjectIds.isEmpty())) {
        m_impl->expandedProjectIds.insert(resolvedProjectId);
    }

    m_impl->projects = snap.projects;
    m_impl->activeProjectId = resolvedProjectId;
    m_impl->allThreads = snap.threads;
    m_impl->activeThreadId = snap.selectedThreadId;
    m_impl->snapshotThreadLinkedFolders = nextSnapshotLinkedFolders;

    if (projectsListChanged || threadsListChanged || snapshotFoldersChanged) {
        m_impl->rebuildProjectsContent();
    } else if (selectedProjectChanged || selectedThreadChanged) {
        m_impl->syncSelectionAndExpansionInPlace();
    }

    // 2. Compute SpellInfoSubpanel values & diff against cached state
    const QString effectiveModel = !snap.modelId.trimmed().isEmpty()
        ? snap.modelId.trimmed()
        : snap.telemetry.modelId.trimmed();
    const bool effectiveConfigured = snap.endpointConfigured || snap.telemetry.endpointConfigured;

    // Resolve active thread tag names
    QStringList tagNames;
    if (!snap.activeThreadTagIds.isEmpty()) {
        QHash<QString, QString> tagIdToName;
        tagIdToName.reserve(snap.tags.size());
        for (const RubricTag& tag : snap.tags) {
            if (!tag.id.isEmpty()) {
                tagIdToName.insert(tag.id, tag.name.trimmed().isEmpty() ? tag.id : tag.name.trimmed());
            }
        }
        for (const QString& tid : snap.activeThreadTagIds) {
            const QString trimmedId = tid.trimmed();
            if (!trimmedId.isEmpty()) {
                tagNames.append(tagIdToName.value(trimmedId, trimmedId));
            }
        }
    }
    const QString nextTagsText = tagNames.join(QStringLiteral(", "));

    // Count armed non-delivered timers
    QVector<const AgentTimer*> armedTimers;
    for (const AgentTimer& timer : snap.activeTimers) {
        if (isArmedTimer(timer)) {
            armedTimers.append(&timer);
        }
    }
    QString nextTimersText;
    if (armedTimers.size() == 1) {
        const QString lbl = armedTimers.first()->label.trimmed();
        nextTimersText = lbl.isEmpty()
            ? QStringLiteral("1 active")
            : QStringLiteral("1 active · %1").arg(lbl);
    } else if (armedTimers.size() > 1) {
        nextTimersText = QStringLiteral("%1 active").arg(armedTimers.size());
    }

    const int nextApprovalCount = static_cast<int>(snap.pendingTerminalApprovals.size());

    const bool infoChanged =
        !m_impl->infoInitialized ||
        (m_impl->endpointConfigured != effectiveConfigured) ||
        (m_impl->engineOnline != snap.telemetry.engineOnline) ||
        (m_impl->endpointBaseUrl != snap.endpointBaseUrl) ||
        (m_impl->engineError != snap.telemetry.engineError) ||
        (m_impl->modelId != effectiveModel) ||
        (m_impl->isAgentBusy != snap.isAgentBusy) ||
        (std::abs(m_impl->decodeTokS - snap.telemetry.decodeTokS) > 0.05) ||
        (m_impl->activePromptTokens != snap.telemetry.activePromptTokens) ||
        (m_impl->activeTagsText != nextTagsText) ||
        (m_impl->armedTimersText != nextTimersText) ||
        (m_impl->pendingTerminalApprovalsCount != nextApprovalCount);

    m_impl->endpointConfigured = effectiveConfigured;
    m_impl->engineOnline = snap.telemetry.engineOnline;
    m_impl->endpointBaseUrl = snap.endpointBaseUrl;
    m_impl->engineError = snap.telemetry.engineError;
    m_impl->modelId = effectiveModel;
    m_impl->isAgentBusy = snap.isAgentBusy;
    m_impl->decodeTokS = snap.telemetry.decodeTokS;
    m_impl->activePromptTokens = snap.telemetry.activePromptTokens;
    m_impl->activeTagsText = nextTagsText;
    m_impl->armedTimersText = nextTimersText;
    m_impl->pendingTerminalApprovalsCount = nextApprovalCount;
    m_impl->infoInitialized = true;

    if (infoChanged) {
        m_impl->updateInfoSubpanelVisuals();
    }

    // 3. Status ticker refresh from active thread events + pushed notices
    if (m_impl->activeEvents != snap.activeEvents) {
        m_impl->activeEvents = snap.activeEvents;
        m_impl->refreshStatusTicker(/*forceRedraw=*/false);
    }
}

void ProjectExplorerPane::applyTheme() {
    m_impl->applyTheme();
}

} // namespace spell
