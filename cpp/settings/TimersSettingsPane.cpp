#include "settings/TimersSettingsPane.h"

#include "core/SpellCoreClient.h"
#include "theme/SpellIcons.h"
#include "theme/SpellTheme.h"
#include "widgets/SpellBadges.h"

#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSize>
#include <QSizePolicy>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>

namespace spell {

namespace {

QString formatDurationHuman(double seconds) {
    const qint64 total = std::max<qint64>(0, static_cast<qint64>(std::ceil(seconds)));
    const qint64 hours = total / 3600;
    const qint64 mins = (total % 3600) / 60;
    const qint64 secs = total % 60;
    if (hours > 0) {
        return QStringLiteral("%1h %2m %3s").arg(hours).arg(mins).arg(secs);
    }
    if (mins > 0) {
        return QStringLiteral("%1m %2s").arg(mins).arg(secs);
    }
    return QStringLiteral("%1s").arg(secs);
}

bool isTimerCancelable(const AgentTimer& timer) {
    if (timer.delivered) {
        return false;
    }
    const QString status = timer.status.trimmed().toLower();
    return status != QLatin1StringView("cancelled") &&
           status != QLatin1StringView("canceled") &&
           status != QLatin1StringView("fired") &&
           status != QLatin1StringView("delivered");
}

QFont makeMonoFont(double pointSize) {
    QFont font = ThemeManager::instance().editorFont();
    font.setStyleHint(QFont::Monospace);
    font.setPointSizeF(pointSize);
    return font;
}

void repolishWidget(QWidget* widget) {
    if (!widget) {
        return;
    }
    if (QStyle* st = widget->style()) {
        st->unpolish(widget);
        st->polish(widget);
    }
    widget->update();
}

struct TimerRowWidgets {
    QString timerId;
    QString label;
    QString note;
    QString watchPath;
    QString threadId;
    QString threadDisplay;
    double durationS = 0.0;
    bool isOtherThread = false;
    bool delivered = false;
    bool canCancel = false;

    QWidget* container = nullptr;
    QLabel* labelWidget = nullptr;
    QLabel* durationMetaLabel = nullptr;
    QLabel* noteLabel = nullptr;
    QLabel* metaLabel = nullptr;
    SpellBadge* statusBadge = nullptr;
    QPushButton* cancelButton = nullptr;
    SpellHairline* separator = nullptr;
};

} // namespace

struct TimersSettingsPane::Impl {
    TimersSettingsPane* owner = nullptr;
    QPointer<SpellCoreClient> client;

    // Local SQLite Wakeup Timers info banner
    QFrame* infoCard = nullptr;
    QLabel* infoIconLabel = nullptr;
    QLabel* infoTitleLabel = nullptr;
    SpellBadge* localBadge = nullptr;
    QLabel* infoBodyLabel = nullptr;

    // Active Timers card
    SpellSectionHeader* activeSectionHeader = nullptr;
    QFrame* activeCard = nullptr;
    QLabel* emptyStateLabel = nullptr;
    QWidget* timersListContainer = nullptr;
    QVBoxLayout* timersListLayout = nullptr;
    SpellHairline* activeFooterHairline = nullptr;
    QPushButton* fireDueButton = nullptr;
    QPushButton* refreshButton = nullptr;
    QLabel* activeSummaryLabel = nullptr;

    // Schedule New Timer form card
    SpellSectionHeader* scheduleSectionHeader = nullptr;
    QFrame* scheduleCard = nullptr;
    QVector<QLabel*> formFieldLabels;
    QLabel* targetThreadValueLabel = nullptr;
    QDoubleSpinBox* durationSpin = nullptr;
    QVector<QPushButton*> presetButtons;
    QLineEdit* labelEdit = nullptr;
    QLineEdit* noteEdit = nullptr;
    QLineEdit* watchPathEdit = nullptr;
    QPushButton* browseWatchButton = nullptr;
    SpellHairline* scheduleFooterHairline = nullptr;
    QLabel* statusLabel = nullptr;
    QPushButton* scheduleButton = nullptr;

    // Cached snapshot state
    QVector<AgentTimer> timers;
    QVector<TimerRowWidgets> timerRows;
    QHash<QString, QString> threadTitleById;
    QString selectedThreadId;
    QString selectedThreadTitle;
    QString defaultWatchPath;
    bool hasReceivedSnapshot = false;
    bool statusIsError = false;

    // Live countdown interpolation timer (runs only while armed timers exist)
    QTimer* countdownTimer = nullptr;
    QElapsedTimer snapshotClock;
    bool hasSnapshotClock = false;

    void setupUi();
    void setStatusMessage(const QString& message, bool isError);
    QString resolveThreadDisplay(const QString& threadId) const;
    void clearTimerRows();
    void rebuildOrUpdateTimerRows(const QVector<AgentTimer>& nextTimers);
    void refreshLiveCountdowns();
    void applyRowTheme(TimerRowWidgets& row, const UiPalette& u);
    void applyTheme();
    void handleFireDueNow();
    void handleCancelTimer(const QString& timerId,
                           const QString& timerLabel,
                           const QPointer<QPushButton>& button);
    void handleBrowseWatchPath();
    void handleScheduleTimer();
};

void TimersSettingsPane::Impl::setupUi() {
    owner->setObjectName(QStringLiteral("timersSettingsPane"));
    owner->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* rootLayout = new QVBoxLayout(owner);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(14);

    // -------------------------------------------------------------------------
    // 1. Local SQLite Wakeup Timers Note Banner
    // -------------------------------------------------------------------------
    infoCard = new QFrame(owner);
    infoCard->setObjectName(QStringLiteral("timersInfoCard"));
    infoCard->setFrameShape(QFrame::NoFrame);
    infoCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* infoLayout = new QVBoxLayout(infoCard);
    infoLayout->setContentsMargins(14, 12, 14, 12);
    infoLayout->setSpacing(6);

    auto* infoHeaderRow = new QHBoxLayout();
    infoHeaderRow->setContentsMargins(0, 0, 0, 0);
    infoHeaderRow->setSpacing(8);

    infoIconLabel = new QLabel(infoCard);
    infoIconLabel->setFixedSize(16, 16);
    infoIconLabel->setAlignment(Qt::AlignCenter);

    infoTitleLabel = new QLabel(QStringLiteral("Local SQLite Wakeup Timers"), infoCard);
    QFont infoTitleFont = infoTitleLabel->font();
    infoTitleFont.setPointSizeF(12.5);
    infoTitleFont.setWeight(QFont::DemiBold);
    infoTitleLabel->setFont(infoTitleFont);

    localBadge = new SpellBadge(
        QStringLiteral("LOCAL SQLITE"), BadgeTone::Info, true, infoCard);

    infoHeaderRow->addWidget(infoIconLabel);
    infoHeaderRow->addWidget(infoTitleLabel);
    infoHeaderRow->addStretch(1);
    infoHeaderRow->addWidget(localBadge);

    infoBodyLabel = new QLabel(
        QStringLiteral(
            "Wakeup timers are persisted and evaluated locally inside BeatrixLeslie IDE's SQLite database. "
            "They run with any configured OpenAI-compatible endpoint or completely offline, performing "
            "zero network calls while idle. Due timers record alarm banners into the target conversation "
            "thread and wake the agent when an endpoint is available."),
        infoCard);
    infoBodyLabel->setWordWrap(true);
    QFont infoBodyFont = infoBodyLabel->font();
    infoBodyFont.setPointSizeF(11.5);
    infoBodyLabel->setFont(infoBodyFont);

    infoLayout->addLayout(infoHeaderRow);
    infoLayout->addWidget(infoBodyLabel);
    rootLayout->addWidget(infoCard);

    // -------------------------------------------------------------------------
    // 2. Active Timers Card
    // -------------------------------------------------------------------------
    auto* activeGroup = new QVBoxLayout();
    activeGroup->setContentsMargins(0, 0, 0, 0);
    activeGroup->setSpacing(6);

    activeSectionHeader = new SpellSectionHeader(
        QStringLiteral("Active Timers (0)"),
        QStringLiteral("bell"),
        BadgeTone::Accent,
        owner);
    activeGroup->addWidget(activeSectionHeader);

    activeCard = new QFrame(owner);
    activeCard->setObjectName(QStringLiteral("timersActiveCard"));
    activeCard->setFrameShape(QFrame::NoFrame);
    activeCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* activeCardLayout = new QVBoxLayout(activeCard);
    activeCardLayout->setContentsMargins(14, 12, 14, 12);
    activeCardLayout->setSpacing(10);

    emptyStateLabel = new QLabel(
        QStringLiteral("No active wakeup timers. Schedule a timer below to wake the active conversation."),
        activeCard);
    emptyStateLabel->setWordWrap(true);
    QFont emptyFont = emptyStateLabel->font();
    emptyFont.setPointSizeF(11.5);
    emptyStateLabel->setFont(emptyFont);

    timersListContainer = new QWidget(activeCard);
    timersListContainer->setObjectName(QStringLiteral("timersListContainer"));
    timersListLayout = new QVBoxLayout(timersListContainer);
    timersListLayout->setContentsMargins(0, 0, 0, 0);
    timersListLayout->setSpacing(0);
    timersListContainer->setVisible(false);

    activeFooterHairline = new SpellHairline(Qt::Horizontal, activeCard);

    auto* activeFooterRow = new QHBoxLayout();
    activeFooterRow->setContentsMargins(0, 0, 0, 0);
    activeFooterRow->setSpacing(8);

    fireDueButton = new QPushButton(QStringLiteral("Fire Due Now"), activeCard);
    fireDueButton->setObjectName(QStringLiteral("timersFireDueButton"));
    fireDueButton->setIconSize(QSize(14, 14));
    fireDueButton->setToolTip(
        QStringLiteral("Immediately check local SQLite timers and fire any overdue wakeup alarms."));

    refreshButton = new QPushButton(QStringLiteral("Refresh"), activeCard);
    refreshButton->setObjectName(QStringLiteral("timersRefreshButton"));
    refreshButton->setIconSize(QSize(14, 14));
    refreshButton->setToolTip(QStringLiteral("Refresh active timers from local SQLite storage."));

    activeSummaryLabel = new QLabel(QStringLiteral("0 active"), activeCard);
    activeSummaryLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    activeFooterRow->addWidget(fireDueButton);
    activeFooterRow->addWidget(refreshButton);
    activeFooterRow->addStretch(1);
    activeFooterRow->addWidget(activeSummaryLabel);

    activeCardLayout->addWidget(emptyStateLabel);
    activeCardLayout->addWidget(timersListContainer);
    activeCardLayout->addWidget(activeFooterHairline);
    activeCardLayout->addLayout(activeFooterRow);

    activeGroup->addWidget(activeCard);
    rootLayout->addLayout(activeGroup);

    // -------------------------------------------------------------------------
    // 3. Schedule New Timer Card
    // -------------------------------------------------------------------------
    auto* scheduleGroup = new QVBoxLayout();
    scheduleGroup->setContentsMargins(0, 0, 0, 0);
    scheduleGroup->setSpacing(6);

    scheduleSectionHeader = new SpellSectionHeader(
        QStringLiteral("Schedule New Timer"),
        QStringLiteral("plus"),
        BadgeTone::Accent,
        owner);
    scheduleGroup->addWidget(scheduleSectionHeader);

    scheduleCard = new QFrame(owner);
    scheduleCard->setObjectName(QStringLiteral("timersScheduleCard"));
    scheduleCard->setFrameShape(QFrame::NoFrame);
    scheduleCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* scheduleCardLayout = new QVBoxLayout(scheduleCard);
    scheduleCardLayout->setContentsMargins(14, 12, 14, 12);
    scheduleCardLayout->setSpacing(10);

    auto* formGrid = new QGridLayout();
    formGrid->setContentsMargins(0, 0, 0, 0);
    formGrid->setHorizontalSpacing(12);
    formGrid->setVerticalSpacing(8);
    formGrid->setColumnStretch(0, 0);
    formGrid->setColumnStretch(1, 1);

    auto makeFieldLabel = [this](const QString& text) -> QLabel* {
        auto* lbl = new QLabel(text, scheduleCard);
        QFont f = lbl->font();
        f.setPointSizeF(12.0);
        lbl->setFont(f);
        lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        lbl->setMinimumWidth(112);
        formFieldLabels.append(lbl);
        return lbl;
    };

    // Row 0: Target conversation thread
    targetThreadValueLabel = new QLabel(QStringLiteral("No active conversation"), scheduleCard);
    targetThreadValueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    targetThreadValueLabel->setWordWrap(true);
    formGrid->addWidget(makeFieldLabel(QStringLiteral("Target thread")), 0, 0);
    formGrid->addWidget(targetThreadValueLabel, 0, 1);

    // Row 1: Duration (seconds) + quick presets
    auto* durationRow = new QHBoxLayout();
    durationRow->setContentsMargins(0, 0, 0, 0);
    durationRow->setSpacing(6);

    durationSpin = new QDoubleSpinBox(scheduleCard);
    durationSpin->setObjectName(QStringLiteral("timersDurationSpin"));
    durationSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    durationSpin->setRange(1.0, 86400.0);
    durationSpin->setDecimals(0);
    durationSpin->setSingleStep(5.0);
    durationSpin->setValue(15.0);
    durationSpin->setSuffix(QStringLiteral(" s"));
    durationSpin->setFixedWidth(116);
    durationSpin->setToolTip(QStringLiteral("Delay in seconds before the local wakeup timer becomes due (1 to 86400)."));
    durationRow->addWidget(durationSpin);

    struct DurationPreset {
        const char* title;
        double seconds;
    };
    static constexpr std::array<DurationPreset, 4> kPresets = {{
        {"15s", 15.0},
        {"60s", 60.0},
        {"5m", 300.0},
        {"15m", 900.0},
    }};
    for (const DurationPreset& preset : kPresets) {
        auto* btn = new QPushButton(QString::fromLatin1(preset.title), scheduleCard);
        btn->setProperty("flat", true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedHeight(26);
        btn->setToolTip(QStringLiteral("Set duration to %1 seconds").arg(static_cast<int>(preset.seconds)));
        const double secs = preset.seconds;
        QObject::connect(btn, &QPushButton::clicked, owner, [this, secs]() {
            if (durationSpin) {
                durationSpin->setValue(secs);
            }
        });
        presetButtons.append(btn);
        durationRow->addWidget(btn);
    }
    durationRow->addStretch(1);

    formGrid->addWidget(makeFieldLabel(QStringLiteral("Duration (sec)")), 1, 0);
    formGrid->addLayout(durationRow, 1, 1);

    // Row 2: Label
    labelEdit = new QLineEdit(scheduleCard);
    labelEdit->setObjectName(QStringLiteral("timersLabelEdit"));
    labelEdit->setText(QStringLiteral("build-audit"));
    labelEdit->setPlaceholderText(QStringLiteral("e.g. build-audit"));
    labelEdit->setClearButtonEnabled(true);
    formGrid->addWidget(makeFieldLabel(QStringLiteral("Label")), 2, 0);
    formGrid->addWidget(labelEdit, 2, 1);

    // Row 3: Note
    noteEdit = new QLineEdit(scheduleCard);
    noteEdit->setObjectName(QStringLiteral("timersNoteEdit"));
    noteEdit->setText(QStringLiteral("Inspect background job & wake idle thread"));
    noteEdit->setPlaceholderText(QStringLiteral("Note recorded when the wakeup alarm fires…"));
    noteEdit->setClearButtonEnabled(true);
    formGrid->addWidget(makeFieldLabel(QStringLiteral("Note")), 3, 0);
    formGrid->addWidget(noteEdit, 3, 1);

    // Row 4: Optional Watch File Path + Browse button
    auto* watchRow = new QHBoxLayout();
    watchRow->setContentsMargins(0, 0, 0, 0);
    watchRow->setSpacing(6);

    watchPathEdit = new QLineEdit(scheduleCard);
    watchPathEdit->setObjectName(QStringLiteral("timersWatchPathEdit"));
    watchPathEdit->setPlaceholderText(QStringLiteral("Optional watch file path (leave empty for none)"));
    watchPathEdit->setClearButtonEnabled(true);

    browseWatchButton = new QPushButton(QStringLiteral("Browse…"), scheduleCard);
    browseWatchButton->setObjectName(QStringLiteral("timersBrowseWatchButton"));
    browseWatchButton->setIconSize(QSize(14, 14));
    browseWatchButton->setToolTip(QStringLiteral("Choose an optional local file to tail when the timer fires."));

    watchRow->addWidget(watchPathEdit, 1);
    watchRow->addWidget(browseWatchButton);

    formGrid->addWidget(makeFieldLabel(QStringLiteral("Watch file")), 4, 0);
    formGrid->addLayout(watchRow, 4, 1);

    scheduleCardLayout->addLayout(formGrid);

    scheduleFooterHairline = new SpellHairline(Qt::Horizontal, scheduleCard);
    scheduleCardLayout->addWidget(scheduleFooterHairline);

    auto* scheduleFooterRow = new QHBoxLayout();
    scheduleFooterRow->setContentsMargins(0, 0, 0, 0);
    scheduleFooterRow->setSpacing(10);

    statusLabel = new QLabel(scheduleCard);
    statusLabel->setWordWrap(true);
    statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusLabel->setVisible(false);

    scheduleButton = new QPushButton(QStringLiteral("Schedule Timer"), scheduleCard);
    scheduleButton->setObjectName(QStringLiteral("timersScheduleButton"));
    scheduleButton->setProperty("primary", true);
    scheduleButton->setIconSize(QSize(14, 14));
    scheduleButton->setToolTip(
        QStringLiteral("Persist a local SQLite wakeup timer for the active conversation thread."));

    scheduleFooterRow->addWidget(statusLabel, 1);
    scheduleFooterRow->addWidget(scheduleButton, 0, Qt::AlignRight | Qt::AlignVCenter);
    scheduleCardLayout->addLayout(scheduleFooterRow);

    scheduleGroup->addWidget(scheduleCard);
    rootLayout->addLayout(scheduleGroup);
    rootLayout->addStretch(1);

    // Local 1s countdown interpolation timer
    countdownTimer = new QTimer(owner);
    countdownTimer->setInterval(1000);
    countdownTimer->setSingleShot(false);
    QObject::connect(countdownTimer, &QTimer::timeout, owner, [this]() {
        refreshLiveCountdowns();
    });

    // Wire button & form actions
    QObject::connect(fireDueButton, &QPushButton::clicked, owner, [this]() {
        handleFireDueNow();
    });
    QObject::connect(refreshButton, &QPushButton::clicked, owner, [this]() {
        if (client) {
            client->refreshSnapshot();
            setStatusMessage(QStringLiteral("Refreshed local timers."), false);
        } else {
            setStatusMessage(QStringLiteral("Core client is not connected."), true);
        }
    });
    QObject::connect(browseWatchButton, &QPushButton::clicked, owner, [this]() {
        handleBrowseWatchPath();
    });
    QObject::connect(scheduleButton, &QPushButton::clicked, owner, [this]() {
        handleScheduleTimer();
    });
    QObject::connect(labelEdit, &QLineEdit::returnPressed, owner, [this]() {
        handleScheduleTimer();
    });
    QObject::connect(noteEdit, &QLineEdit::returnPressed, owner, [this]() {
        handleScheduleTimer();
    });
    QObject::connect(watchPathEdit, &QLineEdit::returnPressed, owner, [this]() {
        handleScheduleTimer();
    });
}

void TimersSettingsPane::Impl::setStatusMessage(const QString& message, bool isError) {
    statusIsError = isError;
    if (!statusLabel) {
        return;
    }
    statusLabel->setText(message);
    statusLabel->setVisible(!message.isEmpty());

    const UiPalette u = ThemeManager::instance().ui();
    const QColor textCol = isError ? u.danger : compositeOver(u.textSecondary, u.background);
    statusLabel->setStyleSheet(
        QStringLiteral("QLabel { color: %1; background-color: transparent; }").arg(qssColor(textCol)));
}

QString TimersSettingsPane::Impl::resolveThreadDisplay(const QString& threadId) const {
    if (threadId.isEmpty()) {
        return {};
    }
    const auto it = threadTitleById.constFind(threadId);
    if (it != threadTitleById.constEnd() && !it.value().trimmed().isEmpty()) {
        return it.value().trimmed();
    }
    return threadId;
}

void TimersSettingsPane::Impl::clearTimerRows() {
    if (timersListLayout) {
        while (QLayoutItem* item = timersListLayout->takeAt(0)) {
            if (QWidget* w = item->widget()) {
                w->hide();
                w->deleteLater();
            }
            delete item;
        }
    }
    timerRows.clear();
}

void TimersSettingsPane::Impl::rebuildOrUpdateTimerRows(const QVector<AgentTimer>& nextTimers) {
    if (activeSectionHeader) {
        activeSectionHeader->setTitle(QStringLiteral("Active Timers (%1)").arg(nextTimers.size()));
    }

    const bool isEmpty = nextTimers.isEmpty();
    if (emptyStateLabel) {
        emptyStateLabel->setVisible(isEmpty);
    }
    if (timersListContainer) {
        timersListContainer->setVisible(!isEmpty);
    }

    // Check if the row structure is identical so 1s countdown updates never flicker or disrupt hover/clicks.
    bool sameStructure = (timerRows.size() == nextTimers.size());
    if (sameStructure) {
        for (int i = 0; i < nextTimers.size(); ++i) {
            const AgentTimer& t = nextTimers[i];
            const TimerRowWidgets& row = timerRows[i];
            const bool otherThread = !t.threadId.isEmpty() && t.threadId != selectedThreadId;
            if (row.timerId != t.id ||
                row.label != t.label ||
                row.note != t.note ||
                row.watchPath != t.watchPath ||
                row.threadId != t.threadId ||
                row.threadDisplay != resolveThreadDisplay(t.threadId) ||
                std::abs(row.durationS - t.durationS) > 0.5 ||
                row.isOtherThread != otherThread ||
                row.delivered != t.delivered ||
                row.canCancel != isTimerCancelable(t)) {
                sameStructure = false;
                break;
            }
        }
    }

    timers = nextTimers;
    snapshotClock.restart();
    hasSnapshotClock = true;

    if (!sameStructure) {
        clearTimerRows();

        const UiPalette u = ThemeManager::instance().ui();
        timerRows.reserve(timers.size());

        for (int i = 0; i < timers.size(); ++i) {
            const AgentTimer& timer = timers[i];
            TimerRowWidgets row;
            row.timerId = timer.id;
            row.label = timer.label;
            row.note = timer.note;
            row.watchPath = timer.watchPath;
            row.threadId = timer.threadId;
            row.threadDisplay = resolveThreadDisplay(timer.threadId);
            row.durationS = timer.durationS;
            row.isOtherThread = !timer.threadId.isEmpty() && timer.threadId != selectedThreadId;
            row.delivered = timer.delivered;
            row.canCancel = isTimerCancelable(timer);

            if (i > 0) {
                row.separator = new SpellHairline(Qt::Horizontal, timersListContainer);
                timersListLayout->addWidget(row.separator);
            }

            row.container = new QWidget(timersListContainer);
            row.container->setObjectName(QStringLiteral("timerRow"));

            auto* rowLayout = new QHBoxLayout(row.container);
            rowLayout->setContentsMargins(0, 8, 0, 8);
            rowLayout->setSpacing(10);

            auto* leftCol = new QVBoxLayout();
            leftCol->setContentsMargins(0, 0, 0, 0);
            leftCol->setSpacing(3);

            auto* titleRow = new QHBoxLayout();
            titleRow->setContentsMargins(0, 0, 0, 0);
            titleRow->setSpacing(8);

            const QString displayLabel =
                timer.label.trimmed().isEmpty() ? QStringLiteral("timer") : timer.label.trimmed();
            row.labelWidget = new QLabel(displayLabel, row.container);
            QFont lblFont = row.labelWidget->font();
            lblFont.setPointSizeF(12.5);
            lblFont.setWeight(QFont::DemiBold);
            row.labelWidget->setFont(lblFont);
            row.labelWidget->setTextInteractionFlags(Qt::TextSelectableByMouse);

            titleRow->addWidget(row.labelWidget);

            if (timer.durationS > 0.0) {
                row.durationMetaLabel = new QLabel(
                    QStringLiteral("(%1)").arg(formatDurationHuman(timer.durationS)),
                    row.container);
                row.durationMetaLabel->setToolTip(
                    QStringLiteral("Configured duration: %1s").arg(QString::number(timer.durationS, 'f', 0)));
                titleRow->addWidget(row.durationMetaLabel);
            }
            titleRow->addStretch(1);
            leftCol->addLayout(titleRow);

            const QString trimmedNote = timer.note.trimmed();
            if (!trimmedNote.isEmpty()) {
                row.noteLabel = new QLabel(trimmedNote, row.container);
                row.noteLabel->setWordWrap(true);
                QFont noteFont = row.noteLabel->font();
                noteFont.setPointSizeF(11.5);
                row.noteLabel->setFont(noteFont);
                row.noteLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
                leftCol->addWidget(row.noteLabel);
            }

            QStringList metaParts;
            if (!timer.watchPath.trimmed().isEmpty()) {
                metaParts.append(QStringLiteral("Watch: %1").arg(timer.watchPath.trimmed()));
            }
            if (!row.threadDisplay.isEmpty() && timer.threadId != selectedThreadId) {
                metaParts.append(QStringLiteral("Thread: %1").arg(row.threadDisplay));
            }
            if (!metaParts.isEmpty()) {
                row.metaLabel = new QLabel(metaParts.join(QStringLiteral("  ·  ")), row.container);
                row.metaLabel->setWordWrap(true);
                row.metaLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
                if (!timer.watchPath.trimmed().isEmpty()) {
                    row.metaLabel->setToolTip(timer.watchPath.trimmed());
                }
                leftCol->addWidget(row.metaLabel);
            }

            rowLayout->addLayout(leftCol, 1);

            auto* rightCol = new QHBoxLayout();
            rightCol->setContentsMargins(0, 0, 0, 0);
            rightCol->setSpacing(6);

            row.statusBadge = new SpellBadge(QStringLiteral("ARMED"), BadgeTone::Info, true, row.container);
            rightCol->addWidget(row.statusBadge, 0, Qt::AlignVCenter);

            if (row.canCancel) {
                row.cancelButton = new QPushButton(QStringLiteral("Cancel"), row.container);
                row.cancelButton->setProperty("danger", true);
                row.cancelButton->setIconSize(QSize(12, 12));
                row.cancelButton->setToolTip(QStringLiteral("Cancel timer \"%1\"").arg(displayLabel));
                const QString capturedId = timer.id;
                const QString capturedLabel = displayLabel;
                const QPointer<QPushButton> btnPtr(row.cancelButton);
                QObject::connect(row.cancelButton, &QPushButton::clicked, owner,
                                 [this, capturedId, capturedLabel, btnPtr]() {
                                     handleCancelTimer(capturedId, capturedLabel, btnPtr);
                                 });
                rightCol->addWidget(row.cancelButton, 0, Qt::AlignVCenter);
            }

            rowLayout->addLayout(rightCol, 0);
            timersListLayout->addWidget(row.container);

            applyRowTheme(row, u);
            timerRows.append(row);
        }
    }

    refreshLiveCountdowns();
}

void TimersSettingsPane::Impl::refreshLiveCountdowns() {
    const double elapsedS = (hasSnapshotClock && snapshotClock.isValid())
                                ? static_cast<double>(snapshotClock.elapsed()) / 1000.0
                                : 0.0;

    int armedCount = 0;
    int dueCount = 0;
    int deliveredCount = 0;
    bool needsTicking = false;

    const int count = std::min(static_cast<int>(timers.size()), static_cast<int>(timerRows.size()));
    for (int i = 0; i < count; ++i) {
        const AgentTimer& timer = timers[i];
        TimerRowWidgets& row = timerRows[i];
        if (!row.statusBadge) {
            continue;
        }

        const QString lowerStatus = timer.status.trimmed().toLower();
        const bool isCancelled = (lowerStatus == QLatin1StringView("cancelled") ||
                                  lowerStatus == QLatin1StringView("canceled"));
        const bool isDelivered = timer.delivered ||
                                 lowerStatus == QLatin1StringView("delivered") ||
                                 lowerStatus == QLatin1StringView("fired");

        if (isDelivered) {
            ++deliveredCount;
            const QString deliveredText =
                (!timer.delivered && lowerStatus == QLatin1StringView("fired"))
                    ? QStringLiteral("FIRED")
                    : QStringLiteral("DELIVERED");
            row.statusBadge->setText(deliveredText);
            row.statusBadge->setTone(BadgeTone::Success);
            row.statusBadge->setShowDot(true);
            row.statusBadge->setToolTip(
                QStringLiteral("Wakeup alarm has been delivered to the conversation."));
            continue;
        }

        if (isCancelled) {
            row.statusBadge->setText(QStringLiteral("CANCELLED"));
            row.statusBadge->setTone(BadgeTone::Neutral);
            row.statusBadge->setShowDot(false);
            row.statusBadge->setToolTip(QStringLiteral("Timer was cancelled."));
            continue;
        }

        const double liveRemaining = std::max(0.0, timer.remainingS - elapsedS);
        if (lowerStatus == QLatin1StringView("due") || liveRemaining <= 0.0) {
            ++dueCount;
            row.statusBadge->setText(QStringLiteral("0s DUE"));
            row.statusBadge->setTone(BadgeTone::Warning);
            row.statusBadge->setShowDot(true);
            row.statusBadge->setToolTip(
                QStringLiteral("Timer is due now. Click \"Fire Due Now\" or wait for the next local poll tick."));
        } else {
            ++armedCount;
            needsTicking = true;
            const qint64 secs = std::max<qint64>(1, static_cast<qint64>(std::llround(liveRemaining)));
            const QString statusUpper =
                lowerStatus.isEmpty() ? QStringLiteral("ARMED") : timer.status.trimmed().toUpper();
            row.statusBadge->setText(QStringLiteral("%1s %2").arg(secs).arg(statusUpper));
            row.statusBadge->setTone(BadgeTone::Info);
            row.statusBadge->setShowDot(true);
            row.statusBadge->setToolTip(
                QStringLiteral("%1 remaining (%2s of %3s)")
                    .arg(formatDurationHuman(static_cast<double>(secs)))
                    .arg(secs)
                    .arg(QString::number(std::max(0.0, timer.durationS), 'f', 0)));
        }
    }

    if (activeSummaryLabel) {
        if (timers.isEmpty()) {
            activeSummaryLabel->setText(QStringLiteral("0 active"));
        } else {
            QStringList parts;
            if (armedCount > 0) {
                parts.append(QStringLiteral("%1 armed").arg(armedCount));
            }
            if (dueCount > 0) {
                parts.append(QStringLiteral("%1 due").arg(dueCount));
            }
            if (deliveredCount > 0) {
                parts.append(QStringLiteral("%1 delivered").arg(deliveredCount));
            }
            if (parts.isEmpty()) {
                parts.append(QStringLiteral("%1 total").arg(timers.size()));
            }
            activeSummaryLabel->setText(parts.join(QStringLiteral(" · ")));
        }
    }

    if (countdownTimer) {
        if (needsTicking) {
            if (!countdownTimer->isActive()) {
                countdownTimer->start();
            }
        } else if (countdownTimer->isActive()) {
            countdownTimer->stop();
        }
    }
}

void TimersSettingsPane::Impl::applyRowTheme(TimerRowWidgets& row, const UiPalette& u) {
    const QColor primaryCol = u.textPrimary;
    const QColor secondaryCol = compositeOver(u.textSecondary, u.background);
    const QColor mutedCol = compositeOver(u.textMuted, u.background);
    const QFont smallMono = makeMonoFont(10.5);

    if (row.labelWidget) {
        row.labelWidget->setStyleSheet(
            QStringLiteral("QLabel { color: %1; background-color: transparent; }").arg(qssColor(primaryCol)));
    }
    if (row.durationMetaLabel) {
        row.durationMetaLabel->setFont(smallMono);
        row.durationMetaLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; background-color: transparent; }").arg(qssColor(mutedCol)));
    }
    if (row.noteLabel) {
        row.noteLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; background-color: transparent; }").arg(qssColor(secondaryCol)));
    }
    if (row.metaLabel) {
        row.metaLabel->setFont(smallMono);
        row.metaLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; background-color: transparent; }").arg(qssColor(mutedCol)));
    }
    if (row.statusBadge) {
        row.statusBadge->applyTheme();
    }
    if (row.cancelButton) {
        row.cancelButton->setIcon(icons::vectorIcon(QStringLiteral("close"), u.danger, 12));
        repolishWidget(row.cancelButton);
    }
    if (row.separator) {
        row.separator->applyTheme();
    }
}

void TimersSettingsPane::Impl::applyTheme() {
    const UiPalette u = ThemeManager::instance().ui();
    const QColor cardBg = u.surface;
    const QColor noteCardBg = compositeOver(u.elevatedSurface, u.background);
    const QColor borderCol = compositeOver(u.subtleBorder, u.background);
    const QColor primaryCol = u.textPrimary;
    const QColor secondaryCol = compositeOver(u.textSecondary, u.background);
    const QColor mutedCol = compositeOver(u.textMuted, u.background);

    const QFont mono11 = makeMonoFont(11.0);
    const QFont mono105 = makeMonoFont(10.5);

    if (infoCard) {
        infoCard->setStyleSheet(
            QStringLiteral(
                "QFrame#timersInfoCard {"
                "  background-color: %1;"
                "  border: 1px solid %2;"
                "  border-radius: 8px;"
                "}")
                .arg(qssColor(noteCardBg), qssColor(borderCol)));
    }
    if (infoIconLabel) {
        infoIconLabel->setPixmap(icons::vectorIcon(QStringLiteral("bell"), primaryCol, 16).pixmap(QSize(16, 16)));
    }
    if (infoTitleLabel) {
        infoTitleLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; background-color: transparent; }").arg(qssColor(primaryCol)));
    }
    if (localBadge) {
        localBadge->applyTheme();
    }
    if (infoBodyLabel) {
        infoBodyLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; background-color: transparent; }").arg(qssColor(secondaryCol)));
    }

    if (activeSectionHeader) {
        activeSectionHeader->applyTheme();
    }
    if (activeCard) {
        activeCard->setStyleSheet(
            QStringLiteral(
                "QFrame#timersActiveCard {"
                "  background-color: %1;"
                "  border: 1px solid %2;"
                "  border-radius: 8px;"
                "}")
                .arg(qssColor(cardBg), qssColor(borderCol)));
    }
    if (emptyStateLabel) {
        emptyStateLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; background-color: transparent; }").arg(qssColor(mutedCol)));
    }
    if (activeFooterHairline) {
        activeFooterHairline->applyTheme();
    }
    if (fireDueButton) {
        fireDueButton->setIcon(icons::vectorIcon(QStringLiteral("bell"), primaryCol, 14));
        repolishWidget(fireDueButton);
    }
    if (refreshButton) {
        refreshButton->setIcon(icons::vectorIcon(QStringLiteral("refresh"), secondaryCol, 14));
        repolishWidget(refreshButton);
    }
    if (activeSummaryLabel) {
        activeSummaryLabel->setFont(mono105);
        activeSummaryLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; background-color: transparent; }").arg(qssColor(mutedCol)));
    }

    if (scheduleSectionHeader) {
        scheduleSectionHeader->applyTheme();
    }
    if (scheduleCard) {
        scheduleCard->setStyleSheet(
            QStringLiteral(
                "QFrame#timersScheduleCard {"
                "  background-color: %1;"
                "  border: 1px solid %2;"
                "  border-radius: 8px;"
                "}")
                .arg(qssColor(cardBg), qssColor(borderCol)));
    }

    for (QLabel* lbl : formFieldLabels) {
        if (lbl) {
            lbl->setStyleSheet(
                QStringLiteral("QLabel { color: %1; background-color: transparent; }").arg(qssColor(secondaryCol)));
        }
    }
    if (targetThreadValueLabel) {
        targetThreadValueLabel->setFont(mono11);
        const QColor targetCol = selectedThreadId.isEmpty() ? mutedCol : primaryCol;
        targetThreadValueLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; background-color: transparent; }").arg(qssColor(targetCol)));
    }
    if (durationSpin) {
        durationSpin->setFont(mono11);
    }
    for (QPushButton* btn : presetButtons) {
        if (btn) {
            btn->setFont(mono105);
            repolishWidget(btn);
        }
    }
    if (watchPathEdit) {
        watchPathEdit->setFont(mono11);
    }
    if (browseWatchButton) {
        browseWatchButton->setIcon(icons::vectorIcon(QStringLiteral("folder"), secondaryCol, 14));
        repolishWidget(browseWatchButton);
    }
    if (scheduleFooterHairline) {
        scheduleFooterHairline->applyTheme();
    }
    if (statusLabel) {
        statusLabel->setFont(mono105);
        const QColor statusCol = statusIsError ? u.danger : secondaryCol;
        statusLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; background-color: transparent; }").arg(qssColor(statusCol)));
    }
    if (scheduleButton) {
        scheduleButton->setIcon(icons::vectorIcon(QStringLiteral("bell"), primaryCol, 14));
        repolishWidget(scheduleButton);
    }

    for (TimerRowWidgets& row : timerRows) {
        applyRowTheme(row, u);
    }
}

void TimersSettingsPane::Impl::handleFireDueNow() {
    if (!client) {
        setStatusMessage(QStringLiteral("Core client is not connected."), true);
        return;
    }
    client->fireDueTimersNow(selectedThreadId);
    setStatusMessage(QStringLiteral("Checked local SQLite timers and fired any due alarms."), false);
}

void TimersSettingsPane::Impl::handleCancelTimer(const QString& timerId,
                                                 const QString& timerLabel,
                                                 const QPointer<QPushButton>& button) {
    if (timerId.trimmed().isEmpty()) {
        return;
    }
    if (!client) {
        setStatusMessage(QStringLiteral("Core client is not connected."), true);
        return;
    }

    if (button) {
        button->setEnabled(false);
        QTimer::singleShot(1500, owner, [button]() {
            if (button) {
                button->setEnabled(true);
            }
        });
    }

    client->cancelTimer(timerId);
    setStatusMessage(QStringLiteral("Cancelled timer \"%1\".").arg(timerLabel), false);
}

void TimersSettingsPane::Impl::handleBrowseWatchPath() {
    QString startDir = defaultWatchPath;
    if (watchPathEdit) {
        const QString current = watchPathEdit->text().trimmed();
        if (!current.isEmpty()) {
            const QFileInfo fi(current);
            if (fi.isDir() && fi.exists()) {
                startDir = fi.absoluteFilePath();
            } else if (fi.dir().exists()) {
                startDir = fi.absolutePath();
            }
        }
    }
    if (startDir.isEmpty() || !QDir(startDir).exists()) {
        startDir = QDir::homePath();
    }

    const QString chosen = QFileDialog::getOpenFileName(
        owner,
        QStringLiteral("Select File to Watch"),
        startDir);
    if (!chosen.isEmpty() && watchPathEdit) {
        watchPathEdit->setText(QDir::toNativeSeparators(chosen));
    }
}

void TimersSettingsPane::Impl::handleScheduleTimer() {
    if (!client) {
        setStatusMessage(QStringLiteral("Core client is not connected."), true);
        return;
    }

    const double durationS = durationSpin ? std::max(1.0, durationSpin->value()) : 15.0;
    QString label = labelEdit ? labelEdit->text().trimmed() : QString();
    if (label.isEmpty()) {
        label = QStringLiteral("wakeup-check");
    }
    QString note = noteEdit ? noteEdit->text().trimmed() : QString();
    if (note.isEmpty()) {
        note = QStringLiteral("Autonomous wakeup check");
    }
    const QString watchPath = watchPathEdit ? watchPathEdit->text().trimmed() : QString();

    if (hasReceivedSnapshot && selectedThreadId.isEmpty()) {
        setStatusMessage(
            QStringLiteral("No active conversation selected — select or create a thread first."),
            true);
        return;
    }

    client->scheduleTimer(selectedThreadId, durationS, label, note, watchPath);

    QString summary = QStringLiteral("Scheduled \"%1\" for %2.")
                          .arg(label, formatDurationHuman(durationS));
    if (!watchPath.isEmpty()) {
        const QString fileName = QFileInfo(watchPath).fileName();
        summary = QStringLiteral("Scheduled \"%1\" for %2 (watching %3).")
                      .arg(label,
                           formatDurationHuman(durationS),
                           fileName.isEmpty() ? watchPath : fileName);
    }
    setStatusMessage(summary, false);
}

// =============================================================================
// TimersSettingsPane public API
// =============================================================================

TimersSettingsPane::TimersSettingsPane(SpellCoreClient* client, QWidget* parent)
    : QWidget(parent)
    , m_impl(std::make_unique<Impl>()) {
    m_impl->owner = this;
    m_impl->client = client;
    m_impl->setupUi();
    applyTheme();

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        applyTheme();
    });

    if (client) {
        connect(client, &SpellCoreClient::snapshotUpdated, this,
                [this](const WorkspaceSnapshot& snap) {
                    updateSnapshot(snap);
                });
        const WorkspaceSnapshot initial = client->currentSnapshot();
        if (!initial.projects.isEmpty() || !initial.threads.isEmpty() || !initial.activeTimers.isEmpty()) {
            updateSnapshot(initial);
        }
    }
}

TimersSettingsPane::~TimersSettingsPane() = default;

void TimersSettingsPane::updateSnapshot(const WorkspaceSnapshot& snap) {
    m_impl->hasReceivedSnapshot = true;

    // Cache thread titles for active target display and cross-thread timer metadata
    m_impl->threadTitleById.clear();
    m_impl->threadTitleById.reserve(snap.threads.size());
    for (const ThreadSummary& thread : snap.threads) {
        if (!thread.id.isEmpty()) {
            m_impl->threadTitleById.insert(thread.id, thread.title);
        }
    }

    m_impl->selectedThreadId = snap.selectedThreadId.trimmed();
    m_impl->selectedThreadTitle = m_impl->resolveThreadDisplay(m_impl->selectedThreadId);

    if (m_impl->targetThreadValueLabel) {
        const UiPalette u = ThemeManager::instance().ui();
        if (m_impl->selectedThreadId.isEmpty()) {
            m_impl->targetThreadValueLabel->setText(
                QStringLiteral("No active conversation selected"));
            m_impl->targetThreadValueLabel->setToolTip(QString());
            m_impl->targetThreadValueLabel->setStyleSheet(
                QStringLiteral("QLabel { color: %1; background-color: transparent; }")
                    .arg(qssColor(compositeOver(u.textMuted, u.background))));
        } else {
            const QString display = m_impl->selectedThreadTitle.isEmpty()
                                        ? m_impl->selectedThreadId
                                        : m_impl->selectedThreadTitle;
            m_impl->targetThreadValueLabel->setText(display);
            m_impl->targetThreadValueLabel->setToolTip(
                QStringLiteral("Thread ID: %1").arg(m_impl->selectedThreadId));
            m_impl->targetThreadValueLabel->setStyleSheet(
                QStringLiteral("QLabel { color: %1; background-color: transparent; }")
                    .arg(qssColor(u.textPrimary)));
        }
    }

    // Resolve default watch directory from the active project
    QString projectRoot;
    for (const ProjectInfo& project : snap.projects) {
        if (project.id == snap.selectedProjectId && !project.rootPath.trimmed().isEmpty()) {
            projectRoot = project.rootPath.trimmed();
            break;
        }
    }
    if (projectRoot.isEmpty() && !snap.projects.isEmpty()) {
        projectRoot = snap.projects.first().rootPath.trimmed();
    }
    m_impl->defaultWatchPath = projectRoot;

    if (m_impl->watchPathEdit) {
        if (!m_impl->defaultWatchPath.isEmpty()) {
            m_impl->watchPathEdit->setPlaceholderText(
                QStringLiteral("Optional watch file path (Browse starts in %1)")
                    .arg(QDir::toNativeSeparators(m_impl->defaultWatchPath)));
        } else {
            m_impl->watchPathEdit->setPlaceholderText(
                QStringLiteral("Optional watch file path (leave empty for none)"));
        }
    }

    m_impl->rebuildOrUpdateTimerRows(snap.activeTimers);
}

void TimersSettingsPane::applyTheme() {
    m_impl->applyTheme();
}

} // namespace spell
