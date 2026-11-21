#include "panes/ActiveSubagentsWidget.h"

#include "theme/SpellIcons.h"
#include "theme/SpellTheme.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QEvent>
#include <QFont>
#include <QFontMetricsF>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLayoutItem>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPointer>
#include <QRectF>
#include <QScreen>
#include <QScrollArea>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace spell {

namespace {

constexpr int kPillPadLeft = 10;
constexpr int kPillPadRight = 10;
constexpr int kPillPadTop = 5;
constexpr int kPillPadBottom = 5;
constexpr int kPillHatSize = 16;
constexpr int kPillSpacing = 6;
constexpr int kPillChevronSize = 9;
constexpr int kPillMinHeight = 26;
constexpr qreal kPillMaxLabelWidth = 180.0;

constexpr int kPopupWidth = 328;
constexpr int kPopupMargin = 12;
constexpr int kPopupMaxScrollHeight = 280;
constexpr int kPopupScrollThreshold = 4;
constexpr int kPopupHeaderHatSize = 16;
constexpr int kPopupRowHatSize = 14;
constexpr int kPopupGapFromPill = 6;
constexpr int kPopupScreenMargin = 8;

constexpr int kPulseTimerIntervalMs = 40;
constexpr double kPulseCyclePeriodMs = 2300.0;
constexpr int kElapsedRefreshIntervalMs = 1000;
constexpr qint64 kPillClickSuppressMs = 80;
constexpr double kTwoPi = 6.28318530717958647692;

QFont makeMonoFont(double pointSize, QFont::Weight weight) {
    QFont font(ThemeManager::instance().editorFontFamily());
    font.setStyleHint(QFont::Monospace);
    font.setPointSizeF(pointSize);
    font.setWeight(weight);
    return font;
}

QFont makeUiFont(double pointSize, QFont::Weight weight) {
    QFont font;
    font.setPointSizeF(pointSize);
    font.setWeight(weight);
    return font;
}

QString humanizeToken(const QString& raw) {
    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty()) {
        return QStringLiteral("Subagent");
    }

    QString normalized = trimmed;
    normalized.replace(QLatin1Char('_'), QLatin1Char(' '));
    normalized.replace(QLatin1Char('-'), QLatin1Char(' '));

    const QStringList parts = normalized.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList formatted;
    formatted.reserve(parts.size());
    for (QString part : parts) {
        if (!part.isEmpty() && part.at(0).isLower()) {
            part[0] = part.at(0).toUpper();
        }
        formatted.append(part);
    }
    return formatted.isEmpty() ? QStringLiteral("Subagent") : formatted.join(QLatin1Char(' '));
}

QString roleDisplayTitle(const QString& rawRole) {
    const QString key = rawRole.trimmed().toLower();
    if (key == QLatin1StringView("prefill")) {
        return QStringLiteral("Prefill Context");
    }
    if (key == QLatin1StringView("coding")) {
        return QStringLiteral("Coding Subagent");
    }
    if (key == QLatin1StringView("critic")) {
        return QStringLiteral("Gatekeeper Critic");
    }
    if (key == QLatin1StringView("timer_triage") ||
        key == QLatin1StringView("timertriage") ||
        key == QLatin1StringView("timer-triage")) {
        return QStringLiteral("Timer Triage");
    }
    return humanizeToken(rawRole);
}

QString shortRoleTitle(const QString& rawRole) {
    const QString key = rawRole.trimmed().toLower();
    if (key == QLatin1StringView("prefill")) {
        return QStringLiteral("Prefill");
    }
    if (key == QLatin1StringView("coding")) {
        return QStringLiteral("Coding");
    }
    if (key == QLatin1StringView("critic")) {
        return QStringLiteral("Critic");
    }
    if (key == QLatin1StringView("timer_triage") ||
        key == QLatin1StringView("timertriage") ||
        key == QLatin1StringView("timer-triage")) {
        return QStringLiteral("Timer Triage");
    }
    return humanizeToken(rawRole);
}

bool isGenericRunningStatus(const QString& status) {
    const QString key = status.trimmed().toLower();
    return key.isEmpty() ||
           key == QLatin1StringView("running") ||
           key == QLatin1StringView("active");
}

bool isErrorStatus(const QString& status) {
    const QString key = status.trimmed().toLower();
    return key.contains(QLatin1StringView("error")) ||
           key.contains(QLatin1StringView("fail")) ||
           key.contains(QLatin1StringView("timeout")) ||
           key.contains(QLatin1StringView("abort"));
}

QString formatStatusBadgeText(const QString& rawStatus) {
    const QString trimmed = rawStatus.trimmed();
    if (trimmed.isEmpty()) {
        return QStringLiteral("RUNNING");
    }
    const bool singleToken = !trimmed.contains(QLatin1Char(' ')) && !trimmed.contains(QLatin1Char('/'));
    if (singleToken) {
        QString upper = trimmed.toUpper();
        upper.replace(QLatin1Char('_'), QLatin1Char(' '));
        return upper;
    }
    return trimmed;
}

QString formatElapsedDuration(qint64 startedAt, qint64 nowMs) {
    if (startedAt <= 0) {
        return QStringLiteral("0s");
    }
    // Accept either Unix milliseconds (e.g. started_at_unix_ms) or Unix seconds.
    const qint64 startedMs = (startedAt >= 1'000'000'000LL && startedAt < 100'000'000'000LL)
                                 ? startedAt * 1000LL
                                 : startedAt;
    const qint64 elapsedSecs = std::max<qint64>(0, (nowMs - startedMs) / 1000LL);
    if (elapsedSecs >= 3600) {
        const qint64 hours = elapsedSecs / 3600;
        const qint64 mins = (elapsedSecs % 3600) / 60;
        return QStringLiteral("%1h %2m")
            .arg(hours)
            .arg(mins, 2, 10, QLatin1Char('0'));
    }
    if (elapsedSecs >= 60) {
        const qint64 mins = elapsedSecs / 60;
        const qint64 secs = elapsedSecs % 60;
        return QStringLiteral("%1m %2s")
            .arg(mins)
            .arg(secs, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1s").arg(elapsedSecs);
}

QString buildPillLabel(const QVector<ActiveSubagentInfo>& subagents) {
    if (subagents.isEmpty()) {
        return {};
    }
    if (subagents.size() > 1) {
        return QStringLiteral("%1 Subagents").arg(subagents.size());
    }
    const ActiveSubagentInfo& first = subagents.first();
    const QString trimmedStatus = first.status.trimmed();
    const QString trimmedLabel = first.label.trimmed();
    const QString trimmedRole = first.role.trimmed();

    if (!isGenericRunningStatus(trimmedStatus)) {
        return QStringLiteral("%1 · %2").arg(shortRoleTitle(trimmedRole), trimmedStatus);
    }
    if (!trimmedLabel.isEmpty() && trimmedLabel.compare(trimmedRole, Qt::CaseInsensitive) != 0) {
        return trimmedLabel;
    }
    if (trimmedRole.isEmpty()) {
        return QStringLiteral("1 Subagent");
    }
    return roleDisplayTitle(trimmedRole);
}

QString buildAccessibleLabel(const QVector<ActiveSubagentInfo>& subagents) {
    if (subagents.isEmpty()) {
        return QStringLiteral("No active subagents");
    }
    if (subagents.size() == 1) {
        const ActiveSubagentInfo& first = subagents.first();
        const QString title = roleDisplayTitle(first.role);
        const QString trimmedStatus = first.status.trimmed();
        if (!isGenericRunningStatus(trimmedStatus)) {
            return QStringLiteral("Active subagent: %1, %2").arg(title, trimmedStatus);
        }
        return QStringLiteral("Active subagent: %1").arg(title);
    }
    return QStringLiteral("%1 active subagents").arg(subagents.size());
}

} // namespace

struct ActiveSubagentsWidget::Impl {
    ActiveSubagentsWidget* owner = nullptr;

    QVector<ActiveSubagentInfo> subagents;
    QString pillText;
    QFont pillFont;

    QPixmap cachedPillHat;
    QColor cachedPillHatColor;
    qreal cachedPillHatDpr = 0.0;

    bool hovered = false;
    bool destroying = false;
    bool popupVisibleNotified = false;
    bool popupDirty = true;

    bool ignoreNextPillClick = false;
    QElapsedTimer ignorePillClickTimer;

    QTimer* pulseTimer = nullptr;
    QElapsedTimer pulseClock;
    double pulseWave = 0.0;

    QTimer* elapsedTimer = nullptr;

    QPointer<QFrame> popup;
    QPointer<QLabel> headerHatLabel;
    QPointer<QLabel> headerTitleLabel;
    QPointer<QLabel> headerRunningLabel;
    QPointer<QFrame> headerDivider;
    QPointer<QScrollArea> scrollArea;
    QPointer<QWidget> rowsContainer;
    QVBoxLayout* rowsLayout = nullptr;
    QVBoxLayout* popupLayout = nullptr;

    struct ElapsedBinding {
        QPointer<QLabel> label;
        qint64 startedAt = 0;
    };
    QVector<ElapsedBinding> elapsedBindings;

    void refreshPillHatPixmap(const QColor& tint) {
        const qreal dpr = icons::detail::currentDevicePixelRatio();
        if (!cachedPillHat.isNull() &&
            cachedPillHatColor == tint &&
            std::abs(cachedPillHatDpr - dpr) < 0.01) {
            return;
        }
        cachedPillHatColor = tint;
        cachedPillHatDpr = dpr;
        cachedPillHat = icons::hatMarkPixmap(kPillHatSize, tint);
    }

    void syncMetadata() {
        pillText = buildPillLabel(subagents);
        owner->setAccessibleName(buildAccessibleLabel(subagents));
        if (subagents.isEmpty()) {
            owner->setToolTip(QStringLiteral("No active subagents"));
        } else if (subagents.size() == 1) {
            const auto& s = subagents.first();
            const QString modelSuffix = s.modelId.trimmed().isEmpty()
                                            ? QString()
                                            : QStringLiteral(" (%1)").arg(s.modelId.trimmed());
            owner->setToolTip(QStringLiteral("%1%2 — click to inspect running subagents")
                                  .arg(roleDisplayTitle(s.role), modelSuffix));
        } else {
            owner->setToolTip(QStringLiteral("%1 active subagents — click to inspect running tasks")
                                  .arg(subagents.size()));
        }
    }

    void startPulseIfNeeded() {
        if (subagents.isEmpty() || destroying) {
            return;
        }
        if (!pulseTimer->isActive()) {
            pulseClock.restart();
            pulseWave = 0.0;
            pulseTimer->start();
        }
    }

    void stopPulse() {
        if (pulseTimer != nullptr && pulseTimer->isActive()) {
            pulseTimer->stop();
        }
        pulseWave = 0.0;
    }

    void notifyPopupVisibility(bool visible) {
        if (destroying || popupVisibleNotified == visible) {
            return;
        }
        popupVisibleNotified = visible;
        if (visible) {
            if (!subagents.isEmpty() && elapsedTimer != nullptr && !elapsedTimer->isActive()) {
                elapsedTimer->start();
            }
        } else {
            if (elapsedTimer != nullptr && elapsedTimer->isActive()) {
                elapsedTimer->stop();
            }
        }
        owner->update();
        emit owner->popupVisibilityChanged(visible);
    }

    void ensurePopupCreated() {
        if (popup != nullptr) {
            return;
        }

        auto* frame = new QFrame(owner, Qt::Popup | Qt::FramelessWindowHint);
        frame->setObjectName(QStringLiteral("SpellActiveSubagentsPopup"));
        frame->setFrameShape(QFrame::NoFrame);
        frame->setFixedWidth(kPopupWidth);
        frame->installEventFilter(owner);

        popupLayout = new QVBoxLayout(frame);
        popupLayout->setContentsMargins(kPopupMargin, kPopupMargin, kPopupMargin, kPopupMargin);
        popupLayout->setSpacing(10);

        // Header row: [Hat] ACTIVE SUBAGENT(S)  ...  [RUNNING]
        auto* headerRow = new QHBoxLayout();
        headerRow->setContentsMargins(0, 0, 0, 0);
        headerRow->setSpacing(7);

        headerHatLabel = new QLabel(frame);
        headerHatLabel->setObjectName(QStringLiteral("SpellSubagentsHeaderHat"));
        headerHatLabel->setFixedSize(kPopupHeaderHatSize, kPopupHeaderHatSize);
        headerHatLabel->setAlignment(Qt::AlignCenter);

        headerTitleLabel = new QLabel(frame);
        headerTitleLabel->setObjectName(QStringLiteral("SpellSubagentsHeaderTitle"));
        headerTitleLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

        headerRunningLabel = new QLabel(QStringLiteral("RUNNING"), frame);
        headerRunningLabel->setObjectName(QStringLiteral("SpellSubagentsHeaderBadge"));
        headerRunningLabel->setAlignment(Qt::AlignCenter);

        headerRow->addWidget(headerHatLabel);
        headerRow->addWidget(headerTitleLabel);
        headerRow->addStretch(1);
        headerRow->addWidget(headerRunningLabel);
        popupLayout->addLayout(headerRow);

        // Hairline divider
        headerDivider = new QFrame(frame);
        headerDivider->setObjectName(QStringLiteral("SpellSubagentsHeaderDivider"));
        headerDivider->setFrameShape(QFrame::NoFrame);
        headerDivider->setFixedHeight(1);
        popupLayout->addWidget(headerDivider);

        // Scrollable rows area (behaves like a snug list for <= 4 rows, scrolls above 4)
        scrollArea = new QScrollArea(frame);
        scrollArea->setObjectName(QStringLiteral("SpellActiveSubagentsScroll"));
        scrollArea->setFrameShape(QFrame::NoFrame);
        scrollArea->setWidgetResizable(true);
        scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        if (scrollArea->viewport() != nullptr) {
            scrollArea->viewport()->setAutoFillBackground(false);
        }

        rowsContainer = new QWidget(scrollArea);
        rowsContainer->setObjectName(QStringLiteral("SpellActiveSubagentsRows"));
        rowsContainer->setAutoFillBackground(false);

        rowsLayout = new QVBoxLayout(rowsContainer);
        rowsLayout->setContentsMargins(0, 0, 0, 0);
        rowsLayout->setSpacing(8);

        scrollArea->setWidget(rowsContainer);
        popupLayout->addWidget(scrollArea);

        popup = frame;
        popupDirty = true;
        applyPopupTheme();
    }

    void clearRows() {
        elapsedBindings.clear();
        if (rowsLayout == nullptr) {
            return;
        }
        while (QLayoutItem* item = rowsLayout->takeAt(0)) {
            if (QWidget* w = item->widget()) {
                w->hide();
                delete w;
            }
            delete item;
        }
    }

    void applyPopupTheme() {
        if (popup == nullptr) {
            return;
        }

        const UiPalette u = ThemeManager::instance().ui();
        const Appearance app = ThemeManager::instance().appearance();
        const QColor solidPopup = (app == Appearance::Dark) ? rgbHex(0x0D0D0D) : rgbHex(0xFAFAFA);
        const QColor solidSurface = u.surface;
        const QColor solidElevated = compositeOver(u.elevatedSurface, solidSurface);
        const QColor solidBorder = compositeOver(u.subtleBorder, solidPopup);
        const QColor solidCardBorder = compositeOver(u.subtleBorder, solidSurface);
        const QColor solidDivider = compositeOver(u.divider, solidPopup);
        const QColor solidSecondary = compositeOver(u.textSecondary, solidSurface);
        const QColor solidMuted = compositeOver(u.textMuted, solidSurface);
        const QColor dangerFill = compositeOver(withAlpha(u.danger, 0.14), solidSurface);
        const QColor dangerBorder = compositeOver(withAlpha(u.danger, 0.55), solidSurface);

        QFont headerFont = makeMonoFont(10.0, QFont::Bold);
        headerFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
        if (headerTitleLabel != nullptr) {
            headerTitleLabel->setFont(headerFont);
        }
        if (headerRunningLabel != nullptr) {
            headerRunningLabel->setFont(makeMonoFont(9.0, QFont::Bold));
        }
        if (headerHatLabel != nullptr) {
            headerHatLabel->setPixmap(icons::hatMarkPixmap(kPopupHeaderHatSize, u.textPrimary));
        }

        const QString qss = QStringLiteral(
            "QFrame#SpellActiveSubagentsPopup {\n"
            "  background-color: %1;\n"
            "  border: 1px solid %2;\n"
            "  border-radius: 8px;\n"
            "}\n"
            "QFrame#SpellSubagentsHeaderDivider {\n"
            "  background-color: %3;\n"
            "  border: none;\n"
            "}\n"
            "QLabel#SpellSubagentsHeaderHat {\n"
            "  background-color: transparent;\n"
            "}\n"
            "QLabel#SpellSubagentsHeaderTitle {\n"
            "  background-color: transparent;\n"
            "  color: %4;\n"
            "}\n"
            "QLabel#SpellSubagentsHeaderBadge {\n"
            "  background-color: transparent;\n"
            "  color: %5;\n"
            "  border: 1px solid %2;\n"
            "  border-radius: 8px;\n"
            "  padding: 2px 7px;\n"
            "}\n"
            "QScrollArea#SpellActiveSubagentsScroll,\n"
            "QScrollArea#SpellActiveSubagentsScroll > QWidget,\n"
            "QWidget#SpellActiveSubagentsRows {\n"
            "  background-color: transparent;\n"
            "  border: none;\n"
            "}\n"
            "QFrame#SpellActiveSubagentRow {\n"
            "  background-color: %6;\n"
            "  border: 1px solid %7;\n"
            "  border-radius: 6px;\n"
            "}\n"
            "QLabel#SpellSubagentRowHat {\n"
            "  background-color: transparent;\n"
            "}\n"
            "QLabel#SpellSubagentRoleLabel {\n"
            "  background-color: transparent;\n"
            "  color: %4;\n"
            "}\n"
            "QLabel#SpellSubagentElapsedLabel {\n"
            "  background-color: transparent;\n"
            "  color: %5;\n"
            "}\n"
            "QLabel#SpellSubagentTaskLabel {\n"
            "  background-color: transparent;\n"
            "  color: %5;\n"
            "}\n"
            "QLabel#SpellSubagentStatusLabel {\n"
            "  background-color: %8;\n"
            "  color: %4;\n"
            "  border: 1px solid %7;\n"
            "  border-radius: 8px;\n"
            "  padding: 2px 7px;\n"
            "}\n"
            "QLabel#SpellSubagentStatusLabel[danger=\"true\"] {\n"
            "  background-color: %9;\n"
            "  color: %10;\n"
            "  border: 1px solid %11;\n"
            "}\n"
            "QLabel#SpellSubagentModelLabel {\n"
            "  background-color: transparent;\n"
            "  color: %12;\n"
            "}\n"
        ).arg(
            qssColor(solidPopup),
            qssColor(solidBorder),
            qssColor(solidDivider),
            qssColor(u.textPrimary),
            qssColor(solidSecondary),
            qssColor(solidSurface),
            qssColor(solidCardBorder),
            qssColor(solidElevated),
            qssColor(dangerFill),
            qssColor(u.danger),
            qssColor(dangerBorder),
            qssColor(solidMuted)
        );

        popup->setStyleSheet(qss);
    }

    void rebuildPopupContent() {
        ensurePopupCreated();
        clearRows();

        const UiPalette u = ThemeManager::instance().ui();
        const QPixmap rowHat = icons::hatMarkPixmap(kPopupRowHatSize, u.textPrimary);
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

        if (headerHatLabel != nullptr) {
            headerHatLabel->setPixmap(icons::hatMarkPixmap(kPopupHeaderHatSize, u.textPrimary));
        }
        if (headerTitleLabel != nullptr) {
            if (subagents.size() == 1) {
                headerTitleLabel->setText(QStringLiteral("ACTIVE SUBAGENT"));
            } else {
                headerTitleLabel->setText(
                    QStringLiteral("ACTIVE SUBAGENTS (%1)").arg(subagents.size()));
            }
        }

        const QFont roleFont = makeUiFont(11.5, QFont::DemiBold);
        const QFont elapsedFont = makeMonoFont(10.0, QFont::DemiBold);
        const QFont taskFont = makeUiFont(11.0, QFont::Normal);
        const QFont statusFont = makeMonoFont(9.5, QFont::DemiBold);
        const QFont modelFont = makeMonoFont(9.5, QFont::Normal);
        const QFontMetricsF roleFm(roleFont);
        const QFontMetricsF statusFm(statusFont);
        const QFontMetricsF modelFm(modelFont);

        const bool needsScroll = subagents.size() > kPopupScrollThreshold;
        const int availListWidth = kPopupWidth - (2 * kPopupMargin) - (needsScroll ? 12 : 0);
        const int cardInnerWidth = std::max(160, availListWidth - 18);
        const qreal maxRoleWidth = std::max<qreal>(100.0, static_cast<qreal>(cardInnerWidth) - 72.0);

        for (const ActiveSubagentInfo& sub : subagents) {
            auto* card = new QFrame(rowsContainer);
            card->setObjectName(QStringLiteral("SpellActiveSubagentRow"));
            card->setFrameShape(QFrame::NoFrame);

            auto* cardLayout = new QVBoxLayout(card);
            cardLayout->setContentsMargins(9, 9, 9, 9);
            cardLayout->setSpacing(5);

            // Top line: [Hat] Role Title  ...  Elapsed
            auto* topRow = new QHBoxLayout();
            topRow->setContentsMargins(0, 0, 0, 0);
            topRow->setSpacing(6);

            auto* hatIcon = new QLabel(card);
            hatIcon->setObjectName(QStringLiteral("SpellSubagentRowHat"));
            hatIcon->setFixedSize(kPopupRowHatSize, kPopupRowHatSize);
            hatIcon->setAlignment(Qt::AlignCenter);
            hatIcon->setPixmap(rowHat);

            const QString displayRole = roleDisplayTitle(sub.role);
            const bool roleElided = roleFm.horizontalAdvance(displayRole) > maxRoleWidth;
            const QString renderedRole = roleElided
                                             ? roleFm.elidedText(displayRole, Qt::ElideRight, maxRoleWidth)
                                             : displayRole;
            auto* roleLabel = new QLabel(renderedRole, card);
            roleLabel->setObjectName(QStringLiteral("SpellSubagentRoleLabel"));
            roleLabel->setFont(roleFont);
            roleLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
            if (roleElided) {
                roleLabel->setToolTip(displayRole);
            }

            auto* elapsedLabel = new QLabel(formatElapsedDuration(sub.startedAt, nowMs), card);
            elapsedLabel->setObjectName(QStringLiteral("SpellSubagentElapsedLabel"));
            elapsedLabel->setFont(elapsedFont);
            elapsedLabel->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
            elapsedBindings.push_back(ElapsedBinding{elapsedLabel, sub.startedAt});

            topRow->addWidget(hatIcon);
            topRow->addWidget(roleLabel, 1);
            topRow->addWidget(elapsedLabel);
            cardLayout->addLayout(topRow);

            // Optional middle line: descriptive task label when distinct from role title
            const QString trimmedLabel = sub.label.trimmed();
            if (!trimmedLabel.isEmpty() &&
                trimmedLabel.compare(sub.role.trimmed(), Qt::CaseInsensitive) != 0 &&
                trimmedLabel.compare(displayRole, Qt::CaseInsensitive) != 0) {
                auto* taskLabel = new QLabel(trimmedLabel, card);
                taskLabel->setObjectName(QStringLiteral("SpellSubagentTaskLabel"));
                taskLabel->setFont(taskFont);
                taskLabel->setWordWrap(true);
                taskLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
                cardLayout->addWidget(taskLabel);
            }

            // Bottom line: [Status Pill]  ...  modelId
            auto* metaRow = new QHBoxLayout();
            metaRow->setContentsMargins(0, 0, 0, 0);
            metaRow->setSpacing(6);

            const QString formattedStatus = formatStatusBadgeText(sub.status);
            auto* statusLabel = new QLabel(formattedStatus, card);
            statusLabel->setObjectName(QStringLiteral("SpellSubagentStatusLabel"));
            statusLabel->setFont(statusFont);
            statusLabel->setAlignment(Qt::AlignCenter);
            if (isErrorStatus(sub.status)) {
                statusLabel->setProperty("danger", true);
            }
            statusLabel->ensurePolished();
            metaRow->addWidget(statusLabel);

            metaRow->addStretch(1);

            const QString trimmedModel = sub.modelId.trimmed();
            if (!trimmedModel.isEmpty()) {
                const qreal statusWidth = std::ceil(statusFm.horizontalAdvance(formattedStatus)) + 18.0;
                const qreal rowMaxModelWidth = std::max<qreal>(
                    80.0, static_cast<qreal>(cardInnerWidth) - statusWidth - 8.0);
                const QString displayedModel = modelFm.horizontalAdvance(trimmedModel) > rowMaxModelWidth
                                                   ? modelFm.elidedText(trimmedModel, Qt::ElideMiddle, rowMaxModelWidth)
                                                   : trimmedModel;
                auto* modelLabel = new QLabel(displayedModel, card);
                modelLabel->setObjectName(QStringLiteral("SpellSubagentModelLabel"));
                modelLabel->setFont(modelFont);
                modelLabel->setToolTip(trimmedModel);
                modelLabel->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
                modelLabel->ensurePolished();
                metaRow->addWidget(modelLabel);
            }

            cardLayout->addLayout(metaRow);
            card->ensurePolished();
            rowsLayout->addWidget(card);
        }

        if (headerRunningLabel != nullptr) {
            headerRunningLabel->ensurePolished();
        }
        popup->ensurePolished();

        scrollArea->setVerticalScrollBarPolicy(
            needsScroll ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);

        rowsLayout->activate();
        int listHeight = rowsLayout->hasHeightForWidth()
                             ? rowsLayout->totalHeightForWidth(availListWidth)
                             : rowsLayout->sizeHint().height();
        if (listHeight <= 0) {
            listHeight = rowsContainer->sizeHint().height();
        }
        const int clampedHeight = needsScroll
                                      ? std::min(kPopupMaxScrollHeight, listHeight)
                                      : listHeight;
        scrollArea->setFixedHeight(std::max(1, clampedHeight));

        popupLayout->activate();
        const int totalHeight = std::max(popupLayout->sizeHint().height(), clampedHeight + 48);
        popup->setFixedSize(kPopupWidth, totalHeight);
        popupDirty = false;
    }

    void refreshElapsedLabels() {
        if (elapsedBindings.isEmpty()) {
            return;
        }
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        for (const ElapsedBinding& binding : elapsedBindings) {
            if (binding.label != nullptr) {
                binding.label->setText(formatElapsedDuration(binding.startedAt, nowMs));
            }
        }
    }

    void positionPopup() {
        if (popup == nullptr) {
            return;
        }
        const QSize popupSize = popup->size();
        const QPoint pillTopLeft = owner->mapToGlobal(QPoint(0, 0));
        const QPoint pillBottomLeft = owner->mapToGlobal(QPoint(0, owner->height()));
        const QPoint pillBottomRight = owner->mapToGlobal(QPoint(owner->width(), owner->height()));

        // Default anchor: right-aligned below the pill.
        int x = pillBottomRight.x() - popupSize.width();
        int y = pillBottomRight.y() + kPopupGapFromPill;

        QScreen* screen = owner->screen();
        if (screen == nullptr) {
            screen = QGuiApplication::screenAt(pillBottomRight);
        }
        if (screen == nullptr) {
            screen = QGuiApplication::primaryScreen();
        }
        if (screen != nullptr) {
            const QRect avail = screen->availableGeometry();
            if (avail.isValid() && !avail.isEmpty()) {
                if (y + popupSize.height() > avail.bottom() - kPopupScreenMargin) {
                    const int aboveY = pillTopLeft.y() - popupSize.height() - kPopupGapFromPill;
                    if (aboveY >= avail.top() + kPopupScreenMargin) {
                        y = aboveY;
                    } else {
                        y = std::max(avail.top() + kPopupScreenMargin,
                                     avail.bottom() - popupSize.height() - kPopupScreenMargin);
                    }
                }
                if (x < avail.left() + kPopupScreenMargin) {
                    x = pillBottomLeft.x();
                }
                const int maxX = std::max(avail.left() + kPopupScreenMargin,
                                          avail.right() - popupSize.width() - kPopupScreenMargin);
                x = std::clamp(x, avail.left() + kPopupScreenMargin, maxX);
            }
        }

        popup->move(x, y);
    }
};

ActiveSubagentsWidget::ActiveSubagentsWidget(QWidget* parent)
    : QWidget(parent)
    , m_impl(std::make_unique<Impl>()) {
    m_impl->owner = this;

    setObjectName(QStringLiteral("ActiveSubagentsWidget"));
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);

    m_impl->pulseTimer = new QTimer(this);
    m_impl->pulseTimer->setInterval(kPulseTimerIntervalMs);
    m_impl->pulseTimer->setTimerType(Qt::CoarseTimer);
    connect(m_impl->pulseTimer, &QTimer::timeout, this, [this]() {
        if (m_impl->subagents.isEmpty()) {
            m_impl->stopPulse();
            return;
        }
        const double elapsedMs = static_cast<double>(m_impl->pulseClock.elapsed());
        const double phase = std::fmod(elapsedMs, kPulseCyclePeriodMs) / kPulseCyclePeriodMs;
        m_impl->pulseWave = 0.5 * (1.0 - std::cos(kTwoPi * phase));
        update();
    });

    m_impl->elapsedTimer = new QTimer(this);
    m_impl->elapsedTimer->setInterval(kElapsedRefreshIntervalMs);
    m_impl->elapsedTimer->setTimerType(Qt::CoarseTimer);
    connect(m_impl->elapsedTimer, &QTimer::timeout, this, [this]() {
        if (!isPopupVisible() || m_impl->subagents.isEmpty()) {
            m_impl->elapsedTimer->stop();
            return;
        }
        m_impl->refreshElapsedLabels();
    });

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        applyTheme();
    });

    m_impl->syncMetadata();
    applyTheme();
    setVisible(false);
}

ActiveSubagentsWidget::~ActiveSubagentsWidget() {
    m_impl->destroying = true;
    m_impl->stopPulse();
    if (m_impl->elapsedTimer != nullptr) {
        m_impl->elapsedTimer->stop();
    }
    if (m_impl->popup != nullptr) {
        m_impl->popup->removeEventFilter(this);
        m_impl->popup->hide();
    }
}

void ActiveSubagentsWidget::setActiveSubagents(const QVector<ActiveSubagentInfo>& list) {
    const bool unchanged = (m_impl->subagents == list);
    if (unchanged && isHidden() == list.isEmpty()) {
        if (!list.isEmpty()) {
            m_impl->startPulseIfNeeded();
            if (isPopupVisible()) {
                m_impl->refreshElapsedLabels();
            }
        }
        return;
    }

    m_impl->subagents = list;
    m_impl->popupDirty = true;
    m_impl->syncMetadata();
    updateGeometry();

    if (m_impl->subagents.isEmpty()) {
        hidePopup();
        m_impl->stopPulse();
        m_impl->hovered = false;
        setVisible(false);
        return;
    }

    setVisible(true);
    m_impl->startPulseIfNeeded();

    if (isPopupVisible()) {
        m_impl->rebuildPopupContent();
        m_impl->positionPopup();
    }
    update();
}

QVector<ActiveSubagentInfo> ActiveSubagentsWidget::activeSubagents() const {
    return m_impl->subagents;
}

int ActiveSubagentsWidget::activeCount() const {
    return static_cast<int>(m_impl->subagents.size());
}

bool ActiveSubagentsWidget::isPopupVisible() const {
    return m_impl->popup != nullptr && m_impl->popup->isVisible();
}

void ActiveSubagentsWidget::showPopup() {
    if (m_impl->subagents.isEmpty()) {
        hidePopup();
        return;
    }

    m_impl->ignoreNextPillClick = false;
    m_impl->ensurePopupCreated();
    if (m_impl->popupDirty) {
        m_impl->rebuildPopupContent();
    } else {
        m_impl->refreshElapsedLabels();
    }
    m_impl->positionPopup();
    m_impl->popup->show();
    m_impl->popup->raise();
    m_impl->notifyPopupVisibility(m_impl->popup->isVisible());
}

void ActiveSubagentsWidget::hidePopup() {
    if (m_impl->popup != nullptr && m_impl->popup->isVisible()) {
        m_impl->popup->hide();
    }
    m_impl->notifyPopupVisibility(false);
}

void ActiveSubagentsWidget::togglePopup() {
    if (isPopupVisible()) {
        hidePopup();
    } else {
        showPopup();
    }
}

void ActiveSubagentsWidget::applyTheme() {
    const UiPalette u = ThemeManager::instance().ui();
    m_impl->pillFont = makeMonoFont(10.5, QFont::DemiBold);
    m_impl->cachedPillHat = QPixmap();
    m_impl->refreshPillHatPixmap(u.textPrimary);

    if (m_impl->popup != nullptr) {
        m_impl->applyPopupTheme();
        if (!m_impl->subagents.isEmpty() && m_impl->popup->isVisible()) {
            m_impl->rebuildPopupContent();
            m_impl->positionPopup();
        } else {
            m_impl->popupDirty = true;
        }
    }

    updateGeometry();
    update();
}

QSize ActiveSubagentsWidget::sizeHint() const {
    const QFontMetricsF fm(m_impl->pillFont);
    const qreal rawTextW = m_impl->pillText.isEmpty()
                               ? 0.0
                               : std::ceil(fm.horizontalAdvance(m_impl->pillText));
    const qreal clampedTextW = std::min(rawTextW, kPillMaxLabelWidth);
    const int contentH = std::max(kPillHatSize, static_cast<int>(std::ceil(fm.height())));
    const int height = std::max(kPillMinHeight, contentH + kPillPadTop + kPillPadBottom);

    int width = kPillPadLeft + kPillHatSize + kPillPadRight + 2;
    if (!m_impl->pillText.isEmpty()) {
        width += kPillSpacing + static_cast<int>(clampedTextW);
    }
    width += kPillSpacing + kPillChevronSize;
    return QSize(width, height);
}

QSize ActiveSubagentsWidget::minimumSizeHint() const {
    const QSize hint = sizeHint();
    const int minCompactW = kPillPadLeft + kPillHatSize + kPillSpacing + 48 +
                            kPillSpacing + kPillChevronSize + kPillPadRight + 2;
    return QSize(std::min(hint.width(), minCompactW), hint.height());
}

void ActiveSubagentsWidget::paintEvent(QPaintEvent* /*event*/) {
    if (m_impl->subagents.isEmpty()) {
        return;
    }

    m_impl->startPulseIfNeeded();

    const UiPalette u = ThemeManager::instance().ui();
    const bool active = m_impl->hovered || isPopupVisible();
    m_impl->refreshPillHatPixmap(u.textPrimary);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRectF pillRect = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = pillRect.height() / 2.0;

    // Monochrome capsule fill
    const QColor fillColor = active
                                 ? compositeOver(u.elevatedSurface, u.background)
                                 : u.background;
    p.setPen(Qt::NoPen);
    p.setBrush(fillColor);
    p.drawRoundedRect(pillRect, radius, radius);

    // Subtle hairline capsule stroke
    const QColor borderColor = active
                                   ? withAlpha(u.textPrimary, 0.45)
                                   : u.subtleBorder;
    p.setPen(QPen(borderColor, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(pillRect, radius, radius);

    const qreal centerY = pillRect.center().y();

    // Pulsing Spell witch hat mark
    if (!m_impl->cachedPillHat.isNull()) {
        const qreal hatOpacity = 1.0 - 0.44 * m_impl->pulseWave;
        const qreal hatScale = 1.04 - 0.12 * m_impl->pulseWave;
        const qreal hatCenterX = static_cast<qreal>(kPillPadLeft) + (kPillHatSize / 2.0);
        const qreal halfHat = kPillHatSize / 2.0;

        p.save();
        p.setOpacity(hatOpacity);
        p.translate(hatCenterX, centerY);
        p.scale(hatScale, hatScale);
        p.drawPixmap(QRectF(-halfHat, -halfHat, kPillHatSize, kPillHatSize),
                     m_impl->cachedPillHat,
                     QRectF(m_impl->cachedPillHat.rect()));
        p.restore();
    }

    // Count / summary label
    const qreal textLeft = static_cast<qreal>(kPillPadLeft + kPillHatSize + kPillSpacing);
    const qreal chevronRight = static_cast<qreal>(width() - kPillPadRight);
    const qreal chevronLeft = chevronRight - static_cast<qreal>(kPillChevronSize);
    const qreal textRight = chevronLeft - static_cast<qreal>(kPillSpacing);
    const qreal availTextW = std::max<qreal>(0.0, textRight - textLeft);

    if (availTextW > 4.0 && !m_impl->pillText.isEmpty()) {
        p.setFont(m_impl->pillFont);
        p.setPen(u.textPrimary);
        const QFontMetricsF fm(m_impl->pillFont);
        const QString elided = fm.elidedText(m_impl->pillText, Qt::ElideRight, availTextW);
        const QRectF textRect(textLeft, pillRect.top(), availTextW, pillRect.height());
        p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, elided);
    }

    // Trailing chevron indicator (up when popup is open, down when closed)
    const qreal cx = chevronLeft + (kPillChevronSize / 2.0);
    const qreal halfW = 3.1;
    const qreal halfH = 1.7;
    QPainterPath chevron;
    if (isPopupVisible()) {
        chevron.moveTo(cx - halfW, centerY + halfH);
        chevron.lineTo(cx, centerY - halfH);
        chevron.lineTo(cx + halfW, centerY + halfH);
    } else {
        chevron.moveTo(cx - halfW, centerY - halfH);
        chevron.lineTo(cx, centerY + halfH);
        chevron.lineTo(cx + halfW, centerY - halfH);
    }
    p.setPen(QPen(active ? u.textSecondary : u.textMuted,
                  1.5,
                  Qt::SolidLine,
                  Qt::RoundCap,
                  Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(chevron);
}

void ActiveSubagentsWidget::mousePressEvent(QMouseEvent* event) {
    if (event != nullptr && event->button() == Qt::LeftButton) {
        if (m_impl->ignoreNextPillClick) {
            m_impl->ignoreNextPillClick = false;
            if (m_impl->ignorePillClickTimer.isValid() &&
                m_impl->ignorePillClickTimer.elapsed() < kPillClickSuppressMs) {
                event->accept();
                return;
            }
        }
        togglePopup();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void ActiveSubagentsWidget::enterEvent(QEnterEvent* event) {
    m_impl->hovered = true;
    update();
    QWidget::enterEvent(event);
}

void ActiveSubagentsWidget::leaveEvent(QEvent* event) {
    m_impl->hovered = false;
    update();
    QWidget::leaveEvent(event);
}

void ActiveSubagentsWidget::hideEvent(QHideEvent* event) {
    m_impl->stopPulse();
    m_impl->hovered = false;
    hidePopup();
    QWidget::hideEvent(event);
}

bool ActiveSubagentsWidget::eventFilter(QObject* watched, QEvent* event) {
    if (m_impl->popup != nullptr && watched == m_impl->popup && event != nullptr) {
        switch (event->type()) {
        case QEvent::Show:
            m_impl->notifyPopupVisibility(true);
            break;
        case QEvent::Hide:
            m_impl->notifyPopupVisibility(false);
            break;
        case QEvent::MouseButtonPress: {
            const auto* mouseEvent = static_cast<QMouseEvent*>(event);
            const QPoint globalPos = mouseEvent->globalPosition().toPoint();
            const QRect pillGlobalRect(mapToGlobal(QPoint(0, 0)), size());
            if (pillGlobalRect.contains(globalPos)) {
                m_impl->ignoreNextPillClick = true;
                m_impl->ignorePillClickTimer.restart();
            }
            break;
        }
        case QEvent::KeyPress: {
            const auto* keyEvent = static_cast<QKeyEvent*>(event);
            if (keyEvent->key() == Qt::Key_Escape) {
                hidePopup();
                return true;
            }
            break;
        }
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace spell
