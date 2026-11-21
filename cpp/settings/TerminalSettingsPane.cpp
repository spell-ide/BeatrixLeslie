#include "settings/TerminalSettingsPane.h"

#include "core/SpellCoreClient.h"
#include "theme/SpellIcons.h"
#include "theme/SpellTheme.h"
#include "widgets/SpellBadges.h"

#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLayoutItem>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QSlider>
#include <QStyle>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <utility>

namespace spell {

namespace {

constexpr double kMinTerminalFontPt = 9.0;
constexpr double kMaxTerminalFontPt = 24.0;
constexpr qint64 kResolvedGraceMs = 3000;

QString formatTimestamp(qint64 rawTimestamp) {
    if (rawTimestamp <= 0) {
        return {};
    }
    const QDateTime dt = (rawTimestamp > 100'000'000'000LL)
                             ? QDateTime::fromMSecsSinceEpoch(rawTimestamp)
                             : QDateTime::fromSecsSinceEpoch(rawTimestamp);
    if (!dt.isValid()) {
        return {};
    }
    return dt.toLocalTime().toString(QStringLiteral("HH:mm:ss"));
}

QString truncateForSummary(const QString& text, qsizetype maxLen = 64) {
    QString singleLine = text.trimmed();
    singleLine.replace(QLatin1Char('\r'), QLatin1Char(' '));
    singleLine.replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (singleLine.size() <= maxLen) {
        return singleLine;
    }
    return singleLine.left(maxLen - 1) + QStringLiteral("…");
}

QString shortenIdentifier(const QString& id, qsizetype maxLen = 12) {
    const QString trimmed = id.trimmed();
    if (trimmed.size() <= maxLen) {
        return trimmed;
    }
    return trimmed.left(maxLen) + QStringLiteral("…");
}

QFrame* createCardFrame(QWidget* parent) {
    auto* frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("spellSettingsCard"));
    frame->setFrameShape(QFrame::NoFrame);
    frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(10);
    return frame;
}

void repolishWidget(QWidget* widget) {
    if (!widget || !widget->style()) {
        return;
    }
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

} // namespace

struct TerminalSettingsPane::Impl {
    struct ApprovalCardRefs {
        QPointer<QFrame> cardFrame;
        QPointer<SpellBadge> kindBadge;
        QPointer<QLabel> metaLabel;
        QPointer<QLabel> commandLabel;
        QPointer<QLabel> reasonLabel;
        QPointer<QPushButton> approveBtn;
        QPointer<QPushButton> denyBtn;
    };

    TerminalSettingsPane* owner = nullptr;
    QPointer<SpellCoreClient> client;

    bool docked = false;
    bool poppedOut = false;

    // Tracked containers & themed widgets
    QVector<QFrame*> cards;
    QVector<SpellSectionHeader*> sectionHeaders;
    QVector<SpellHairline*> hairlines;
    QVector<QLabel*> captionLabels;
    QVector<QLabel*> mutedLabels;

    // Section 1: Integrated Terminal status & placement controls
    SpellBadge* statusBadge = nullptr;
    SpellValueRow* placementRow = nullptr;
    SpellValueRow* directoryRow = nullptr;
    QPushButton* toggleDockedBtn = nullptr;
    QPushButton* popOutBtn = nullptr;
    QPushButton* clearBufferBtn = nullptr;
    QPushButton* restartShellBtn = nullptr;

    // Section 2: Quick Terminal Theme & Font Size
    QComboBox* themeCombo = nullptr;
    QSlider* fontSizeSlider = nullptr;
    QDoubleSpinBox* fontSizeSpin = nullptr;
    QFrame* previewFrame = nullptr;
    QLabel* previewLabel = nullptr;

    // Section 3: Agent Access & Pending type_in_terminal Approvals
    SpellValueRow* readAccessRow = nullptr;
    SpellValueRow* writeAccessRow = nullptr;
    QLabel* approvalsHeaderLabel = nullptr;
    SpellBadge* pendingCountBadge = nullptr;
    QPushButton* denyAllBtn = nullptr;
    QLabel* feedbackLabel = nullptr;
    bool feedbackIsApproval = true;
    QWidget* approvalsContainer = nullptr;
    QVBoxLayout* approvalsLayout = nullptr;
    QLabel* emptyApprovalsLabel = nullptr;

    QVector<TerminalApprovalRequest> pendingApprovals;
    QVector<ApprovalCardRefs> approvalCards;
    QHash<QString, qint64> recentlyResolvedIds;

    void setupUi();
    void updateStateDisplay();
    void syncThemeControlsFromManager();
    void updateTerminalPreview();
    void rebuildApprovalsList();
    void styleApprovalCard(const ApprovalCardRefs& refs, const UiPalette& u);
    void upsertPendingApproval(const TerminalApprovalRequest& req);
    void resolveApproval(const QString& approvalId, bool approved);
    void denyAllPendingApprovals();
    void applyFeedbackStyle(const UiPalette& u);
};

void TerminalSettingsPane::Impl::setupUi() {
    owner->setObjectName(QStringLiteral("TerminalSettingsPane"));
    owner->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* root = new QVBoxLayout(owner);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(14);

    // =========================================================================
    // Section 1: Message Terminal — Status & Placement Controls
    // =========================================================================
    {
        auto* header = new SpellSectionHeader(
            QStringLiteral("Message Terminal"), QStringLiteral("terminal"), BadgeTone::Accent, owner);
        sectionHeaders.append(header);
        root->addWidget(header);

        QFrame* card = createCardFrame(owner);
        cards.append(card);
        auto* cardLayout = qobject_cast<QVBoxLayout*>(card->layout());

        // Status row
        auto* statusRow = new QHBoxLayout();
        statusRow->setContentsMargins(0, 0, 0, 0);
        statusRow->setSpacing(10);
        auto* statusCaption = new QLabel(QStringLiteral("Status"), card);
        captionLabels.append(statusCaption);
        statusBadge = new SpellBadge(QStringLiteral("HIDDEN"), BadgeTone::Neutral, true, card);
        statusBadge->setObjectName(QStringLiteral("terminalStatusBadge"));
        statusRow->addWidget(statusCaption);
        statusRow->addStretch(1);
        statusRow->addWidget(statusBadge);
        cardLayout->addLayout(statusRow);

        // Placement & directory rows
        placementRow = new SpellValueRow(
            QStringLiteral("Placement"), QStringLiteral("Hidden"), BadgeTone::Neutral, card);
        placementRow->setObjectName(QStringLiteral("terminalPlacementRow"));
        placementRow->setValueIsPrimary(true);
        cardLayout->addWidget(placementRow);

        directoryRow = new SpellValueRow(
            QStringLiteral("Workspace directory"), QStringLiteral("—"), BadgeTone::Neutral, card);
        directoryRow->setObjectName(QStringLiteral("terminalDirectoryRow"));
        directoryRow->setValueIsPrimary(true);
        if (QLabel* valLbl = directoryRow->valueWidget()) {
            valLbl->setWordWrap(true);
        }
        cardLayout->addWidget(directoryRow);

        auto* divider = new SpellHairline(Qt::Horizontal, card);
        hairlines.append(divider);
        cardLayout->addWidget(divider);

        // Placement action buttons row
        auto* placementBtnRow = new QHBoxLayout();
        placementBtnRow->setContentsMargins(0, 0, 0, 0);
        placementBtnRow->setSpacing(8);

        toggleDockedBtn = new QPushButton(QStringLiteral("Toggle Docked Terminal"), card);
        toggleDockedBtn->setObjectName(QStringLiteral("toggleDockedTerminalButton"));
        toggleDockedBtn->setCursor(Qt::PointingHandCursor);
        toggleDockedBtn->setIconSize(QSize(14, 14));
        QObject::connect(toggleDockedBtn, &QPushButton::clicked, owner, [this]() {
            emit owner->toggleDockedRequested();
        });

        popOutBtn = new QPushButton(QStringLiteral("Pop Out"), card);
        popOutBtn->setObjectName(QStringLiteral("togglePopOutTerminalButton"));
        popOutBtn->setCursor(Qt::PointingHandCursor);
        popOutBtn->setIconSize(QSize(14, 14));
        QObject::connect(popOutBtn, &QPushButton::clicked, owner, [this]() {
            emit owner->togglePopOutRequested();
        });

        placementBtnRow->addWidget(toggleDockedBtn);
        placementBtnRow->addWidget(popOutBtn);
        placementBtnRow->addStretch(1);
        cardLayout->addLayout(placementBtnRow);

        // Buffer & shell action buttons row
        auto* shellBtnRow = new QHBoxLayout();
        shellBtnRow->setContentsMargins(0, 0, 0, 0);
        shellBtnRow->setSpacing(8);

        clearBufferBtn = new QPushButton(QStringLiteral("Clear Buffer"), card);
        clearBufferBtn->setObjectName(QStringLiteral("clearTerminalBufferButton"));
        clearBufferBtn->setCursor(Qt::PointingHandCursor);
        clearBufferBtn->setIconSize(QSize(14, 14));
        clearBufferBtn->setToolTip(QStringLiteral("Clear the terminal screen and scrollback buffer"));
        QObject::connect(clearBufferBtn, &QPushButton::clicked, owner, [this]() {
            emit owner->clearTerminalRequested();
        });

        restartShellBtn = new QPushButton(QStringLiteral("Restart Shell"), card);
        restartShellBtn->setObjectName(QStringLiteral("restartTerminalShellButton"));
        restartShellBtn->setCursor(Qt::PointingHandCursor);
        restartShellBtn->setIconSize(QSize(14, 14));
        restartShellBtn->setToolTip(
            QStringLiteral("Restart the interactive shell in the active workspace directory"));
        QObject::connect(restartShellBtn, &QPushButton::clicked, owner, [this]() {
            emit owner->restartTerminalRequested();
        });

        shellBtnRow->addWidget(clearBufferBtn);
        shellBtnRow->addWidget(restartShellBtn);
        shellBtnRow->addStretch(1);
        cardLayout->addLayout(shellBtnRow);

        root->addWidget(card);
    }

    // =========================================================================
    // Section 2: Quick Terminal Theme & Font Size (synced with ThemeManager)
    // =========================================================================
    {
        auto* header = new SpellSectionHeader(
            QStringLiteral("Terminal Appearance"), QStringLiteral("paintbrush"), BadgeTone::Accent, owner);
        sectionHeaders.append(header);
        root->addWidget(header);

        QFrame* card = createCardFrame(owner);
        cards.append(card);
        auto* cardLayout = qobject_cast<QVBoxLayout*>(card->layout());

        // Theme selector row
        auto* themeRow = new QHBoxLayout();
        themeRow->setContentsMargins(0, 0, 0, 0);
        themeRow->setSpacing(10);

        auto* themeCaption = new QLabel(QStringLiteral("Terminal theme"), card);
        captionLabels.append(themeCaption);

        themeCombo = new QComboBox(card);
        themeCombo->setObjectName(QStringLiteral("terminalThemeCombo"));
        themeCombo->setMinimumWidth(210);
        themeCombo->setCursor(Qt::PointingHandCursor);
        for (const TerminalThemeId id : ThemeManager::allTerminalThemes()) {
            themeCombo->addItem(ThemeManager::terminalThemeName(id), static_cast<int>(id));
        }
        QObject::connect(themeCombo, &QComboBox::currentIndexChanged, owner, [this](int index) {
            if (index < 0 || !themeCombo) {
                return;
            }
            const QVariant data = themeCombo->itemData(index);
            if (data.isValid()) {
                ThemeManager::instance().setTerminalTheme(static_cast<TerminalThemeId>(data.toInt()));
            }
        });

        themeRow->addWidget(themeCaption);
        themeRow->addStretch(1);
        themeRow->addWidget(themeCombo);
        cardLayout->addLayout(themeRow);

        // Font size selector row
        auto* fontRow = new QHBoxLayout();
        fontRow->setContentsMargins(0, 0, 0, 0);
        fontRow->setSpacing(10);

        auto* fontCaption = new QLabel(QStringLiteral("Font size"), card);
        captionLabels.append(fontCaption);

        fontSizeSlider = new QSlider(Qt::Horizontal, card);
        fontSizeSlider->setObjectName(QStringLiteral("terminalFontSizeSlider"));
        fontSizeSlider->setRange(static_cast<int>(std::lround(kMinTerminalFontPt * 2.0)),
                                 static_cast<int>(std::lround(kMaxTerminalFontPt * 2.0)));
        fontSizeSlider->setSingleStep(1);
        fontSizeSlider->setPageStep(2);
        fontSizeSlider->setFixedWidth(150);
        fontSizeSlider->setCursor(Qt::PointingHandCursor);
        QObject::connect(fontSizeSlider, &QSlider::valueChanged, owner, [](int halfPoints) {
            ThemeManager::instance().setTerminalFontSize(static_cast<double>(halfPoints) / 2.0);
        });

        fontSizeSpin = new QDoubleSpinBox(card);
        fontSizeSpin->setObjectName(QStringLiteral("terminalFontSizeSpin"));
        fontSizeSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        fontSizeSpin->setRange(kMinTerminalFontPt, kMaxTerminalFontPt);
        fontSizeSpin->setSingleStep(0.5);
        fontSizeSpin->setDecimals(1);
        fontSizeSpin->setSuffix(QStringLiteral(" pt"));
        fontSizeSpin->setFixedWidth(88);
        QObject::connect(fontSizeSpin, &QDoubleSpinBox::valueChanged, owner, [](double pt) {
            ThemeManager::instance().setTerminalFontSize(pt);
        });

        fontRow->addWidget(fontCaption);
        fontRow->addStretch(1);
        fontRow->addWidget(fontSizeSlider);
        fontRow->addWidget(fontSizeSpin);
        cardLayout->addLayout(fontRow);

        // Live ANSI palette & font preview
        previewFrame = new QFrame(card);
        previewFrame->setObjectName(QStringLiteral("terminalPreviewFrame"));
        previewFrame->setFrameShape(QFrame::NoFrame);
        auto* previewLayout = new QVBoxLayout(previewFrame);
        previewLayout->setContentsMargins(12, 10, 12, 10);
        previewLayout->setSpacing(0);

        previewLabel = new QLabel(previewFrame);
        previewLabel->setObjectName(QStringLiteral("terminalPreviewLabel"));
        previewLabel->setTextFormat(Qt::RichText);
        previewLabel->setWordWrap(true);
        previewLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        previewLayout->addWidget(previewLabel);

        cardLayout->addWidget(previewFrame);
        root->addWidget(card);
    }

    // =========================================================================
    // Section 3: Agent Access & Pending type_in_terminal Approvals
    // =========================================================================
    {
        auto* header = new SpellSectionHeader(
            QStringLiteral("Agent Access & Approvals"), QStringLiteral("shield"), BadgeTone::Accent, owner);
        sectionHeaders.append(header);
        root->addWidget(header);

        QFrame* card = createCardFrame(owner);
        cards.append(card);
        auto* cardLayout = qobject_cast<QVBoxLayout*>(card->layout());

        readAccessRow = new SpellValueRow(
            QStringLiteral("Read output (read_terminal)"),
            QStringLiteral("Allowed"),
            BadgeTone::Info,
            card);
        readAccessRow->setObjectName(QStringLiteral("readTerminalAccessRow"));
        cardLayout->addWidget(readAccessRow);

        writeAccessRow = new SpellValueRow(
            QStringLiteral("Type into shell (type_in_terminal)"),
            QStringLiteral("Approval every time"),
            BadgeTone::Warning,
            card);
        writeAccessRow->setObjectName(QStringLiteral("typeInTerminalAccessRow"));
        cardLayout->addWidget(writeAccessRow);

        auto* handoffHint = new QLabel(
            QStringLiteral("Type #spell <message> in the terminal and press Return to send your message "
                           "and recent output directly to the active BeatrixLeslie IDE conversation."),
            card);
        handoffHint->setWordWrap(true);
        mutedLabels.append(handoffHint);
        cardLayout->addWidget(handoffHint);

        auto* divider = new SpellHairline(Qt::Horizontal, card);
        hairlines.append(divider);
        cardLayout->addWidget(divider);

        // Pending approvals header bar
        auto* pendingBar = new QHBoxLayout();
        pendingBar->setContentsMargins(0, 0, 0, 0);
        pendingBar->setSpacing(8);

        approvalsHeaderLabel = new QLabel(QStringLiteral("PENDING TYPE_IN_TERMINAL APPROVALS"), card);
        approvalsHeaderLabel->setObjectName(QStringLiteral("pendingApprovalsHeaderLabel"));

        pendingCountBadge = new SpellBadge(QStringLiteral("0 PENDING"), BadgeTone::Neutral, false, card);
        pendingCountBadge->setObjectName(QStringLiteral("pendingApprovalsCountBadge"));

        denyAllBtn = new QPushButton(QStringLiteral("Deny All"), card);
        denyAllBtn->setObjectName(QStringLiteral("denyAllTerminalApprovalsButton"));
        denyAllBtn->setProperty("danger", true);
        denyAllBtn->setCursor(Qt::PointingHandCursor);
        denyAllBtn->setIconSize(QSize(13, 13));
        denyAllBtn->setVisible(false);
        QObject::connect(denyAllBtn, &QPushButton::clicked, owner, [this]() {
            denyAllPendingApprovals();
        });

        pendingBar->addWidget(approvalsHeaderLabel);
        pendingBar->addWidget(pendingCountBadge);
        pendingBar->addStretch(1);
        pendingBar->addWidget(denyAllBtn);
        cardLayout->addLayout(pendingBar);

        // Transient resolution feedback label
        feedbackLabel = new QLabel(card);
        feedbackLabel->setObjectName(QStringLiteral("terminalApprovalFeedbackLabel"));
        feedbackLabel->setWordWrap(true);
        feedbackLabel->setVisible(false);
        cardLayout->addWidget(feedbackLabel);

        // Dynamic pending approval cards container
        approvalsContainer = new QWidget(card);
        approvalsContainer->setObjectName(QStringLiteral("pendingApprovalsContainer"));
        approvalsContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        approvalsLayout = new QVBoxLayout(approvalsContainer);
        approvalsLayout->setContentsMargins(0, 2, 0, 0);
        approvalsLayout->setSpacing(8);
        cardLayout->addWidget(approvalsContainer);

        root->addWidget(card);
    }

    root->addStretch(1);
}

void TerminalSettingsPane::Impl::updateStateDisplay() {
    if (poppedOut) {
        statusBadge->setText(QStringLiteral("POPPED OUT"));
        statusBadge->setTone(BadgeTone::Info);
        statusBadge->setShowDot(true);

        placementRow->setValue(QStringLiteral("Standalone Window"));
        placementRow->setTone(BadgeTone::Info);

        popOutBtn->setText(QStringLiteral("Dock Back"));
        popOutBtn->setToolTip(
            QStringLiteral("Return the popped-out terminal window back into the BeatrixLeslie IDE workspace"));
        toggleDockedBtn->setProperty("primary", false);
        toggleDockedBtn->setToolTip(
            QStringLiteral("Toggle the docked integrated terminal panel in the workspace"));
    } else if (docked) {
        statusBadge->setText(QStringLiteral("DOCKED"));
        statusBadge->setTone(BadgeTone::Success);
        statusBadge->setShowDot(true);

        placementRow->setValue(QStringLiteral("Docked in Workspace"));
        placementRow->setTone(BadgeTone::Success);

        popOutBtn->setText(QStringLiteral("Pop Out"));
        popOutBtn->setToolTip(
            QStringLiteral("Detach the live terminal into a standalone top-level window"));
        toggleDockedBtn->setProperty("primary", false);
        toggleDockedBtn->setToolTip(QStringLiteral("Hide the docked integrated terminal panel"));
    } else {
        statusBadge->setText(QStringLiteral("HIDDEN"));
        statusBadge->setTone(BadgeTone::Neutral);
        statusBadge->setShowDot(true);

        placementRow->setValue(QStringLiteral("Hidden"));
        placementRow->setTone(BadgeTone::Neutral);

        popOutBtn->setText(QStringLiteral("Pop Out"));
        popOutBtn->setToolTip(
            QStringLiteral("Open the integrated terminal in a standalone top-level window"));
        toggleDockedBtn->setProperty("primary", true);
        toggleDockedBtn->setToolTip(
            QStringLiteral("Show the integrated terminal docked inside the workspace"));
    }

    repolishWidget(toggleDockedBtn);
    repolishWidget(popOutBtn);
}

void TerminalSettingsPane::Impl::syncThemeControlsFromManager() {
    const auto& tm = ThemeManager::instance();

    if (themeCombo) {
        const QSignalBlocker blocker(themeCombo);
        const int targetId = static_cast<int>(tm.terminalTheme());
        const int idx = themeCombo->findData(targetId);
        if (idx >= 0 && themeCombo->currentIndex() != idx) {
            themeCombo->setCurrentIndex(idx);
        }
    }

    const double pt = std::clamp(tm.terminalFontSize(), kMinTerminalFontPt, kMaxTerminalFontPt);
    if (fontSizeSlider) {
        const QSignalBlocker blocker(fontSizeSlider);
        const int halfPts = static_cast<int>(std::lround(pt * 2.0));
        if (fontSizeSlider->value() != halfPts) {
            fontSizeSlider->setValue(halfPts);
        }
    }
    if (fontSizeSpin) {
        const QSignalBlocker blocker(fontSizeSpin);
        if (std::abs(fontSizeSpin->value() - pt) > 1e-4) {
            fontSizeSpin->setValue(pt);
        }
    }
}

void TerminalSettingsPane::Impl::updateTerminalPreview() {
    if (!previewFrame || !previewLabel) {
        return;
    }

    const auto& tm = ThemeManager::instance();
    const UiPalette u = tm.ui();
    const TerminalPalette tp = tm.terminal();
    const QColor borderCol = compositeOver(u.subtleBorder, u.background);

    previewFrame->setStyleSheet(QStringLiteral(
        "QFrame#terminalPreviewFrame {"
        "  background-color: %1;"
        "  border: 1px solid %2;"
        "  border-radius: 6px;"
        "}")
                                    .arg(qssColor(tp.background), qssColor(borderCol)));

    previewLabel->setFont(tm.terminalFont());
    previewLabel->setStyleSheet(QStringLiteral(
        "QLabel#terminalPreviewLabel {"
        "  background-color: transparent;"
        "  border: none;"
        "  color: %1;"
        "}")
                                    .arg(qssColor(tp.foreground)));

    const QString green = tp.ansi[2].name(QColor::HexRgb);
    const QString yellow = tp.ansi[3].name(QColor::HexRgb);
    const QString blue = tp.ansi[4].name(QColor::HexRgb);
    const QString fg = tp.foreground.name(QColor::HexRgb);
    const QString bold = tp.bold.name(QColor::HexRgb);
    const QString cursor = tp.cursor.name(QColor::HexRgb);

    const QString html = QStringLiteral(
        "<div style=\"line-height: 1.38;\">"
        "<span style=\"color:%1;\">spell@host</span>"
        "<span style=\"color:%2;\"> ~/workspace </span>"
        "<span style=\"color:%3;\">% </span>"
        "<span style=\"color:%4; font-weight:600;\">cargo test</span><br/>"
        "<span style=\"color:%1;\">test result: ok. 42 passed; 0 failed</span><br/>"
        "<span style=\"color:%5;\">warning: unused variable `x`</span><br/>"
        "<span style=\"color:%6; font-weight:600;\">#spell </span>"
        "<span style=\"color:%3;\">explain the failing build target</span>"
        "</div>")
                             .arg(green, blue, fg, bold, yellow, cursor);

    previewLabel->setText(html);
}

void TerminalSettingsPane::Impl::styleApprovalCard(const ApprovalCardRefs& refs, const UiPalette& u) {
    const QColor cardBg = u.surface;
    const QColor borderCol = compositeOver(u.subtleBorder, u.background);
    const QColor secondaryCol = compositeOver(u.textSecondary, u.background);
    const QColor mutedCol = compositeOver(u.textMuted, u.background);
    const QColor approveFill = withAlpha(u.diffAdded, 0.14);
    const QColor approveHover = withAlpha(u.diffAdded, 0.24);
    const QColor approveBorder = withAlpha(u.diffAdded, 0.70);
    const QColor dangerFill = withAlpha(u.danger, 0.14);
    const QColor dangerHover = withAlpha(u.danger, 0.24);
    const QColor dangerBorder = withAlpha(u.danger, 0.70);

    if (refs.cardFrame) {
        refs.cardFrame->setStyleSheet(QStringLiteral(
            "QFrame#terminalApprovalCard {"
            "  background-color: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 6px;"
            "}")
                                          .arg(qssColor(cardBg), qssColor(borderCol)));
    }
    if (refs.kindBadge) {
        refs.kindBadge->applyTheme();
    }
    if (refs.metaLabel) {
        refs.metaLabel->setStyleSheet(QStringLiteral(
            "QLabel { background: transparent; color: %1; font-size: 10.5px; }")
                                          .arg(qssColor(mutedCol)));
    }
    if (refs.commandLabel) {
        QFont cmdFont = ThemeManager::instance().editorFont();
        cmdFont.setPointSizeF(11.0);
        refs.commandLabel->setFont(cmdFont);
        refs.commandLabel->setStyleSheet(QStringLiteral(
            "QLabel#terminalApprovalCommandLabel {"
            "  background-color: %1;"
            "  color: %2;"
            "  border: 1px solid %3;"
            "  border-radius: 5px;"
            "  padding: 6px 8px;"
            "}")
                                             .arg(qssColor(u.background),
                                                  qssColor(u.textPrimary),
                                                  qssColor(borderCol)));
    }
    if (refs.reasonLabel) {
        refs.reasonLabel->setStyleSheet(QStringLiteral(
            "QLabel { background: transparent; color: %1; font-size: 11px; }")
                                            .arg(qssColor(secondaryCol)));
    }
    if (refs.approveBtn) {
        refs.approveBtn->setIcon(icons::vectorIcon(QStringLiteral("check"), u.diffAdded, 14));
        refs.approveBtn->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background-color: %1;"
            "  color: %2;"
            "  border: 1px solid %3;"
            "  border-radius: 6px;"
            "  padding: 4px 12px;"
            "  font-weight: 600;"
            "}"
            "QPushButton:hover {"
            "  background-color: %4;"
            "}"
            "QPushButton:pressed {"
            "  background-color: %4;"
            "}")
                                           .arg(qssColor(approveFill),
                                                qssColor(u.diffAdded),
                                                qssColor(approveBorder),
                                                qssColor(approveHover)));
    }
    if (refs.denyBtn) {
        refs.denyBtn->setIcon(icons::vectorIcon(QStringLiteral("close"), u.danger, 14));
        refs.denyBtn->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background-color: %1;"
            "  color: %2;"
            "  border: 1px solid %3;"
            "  border-radius: 6px;"
            "  padding: 4px 12px;"
            "  font-weight: 600;"
            "}"
            "QPushButton:hover {"
            "  background-color: %4;"
            "}"
            "QPushButton:pressed {"
            "  background-color: %4;"
            "}")
                                        .arg(qssColor(dangerFill),
                                             qssColor(u.danger),
                                             qssColor(dangerBorder),
                                             qssColor(dangerHover)));
    }
}

void TerminalSettingsPane::Impl::rebuildApprovalsList() {
    if (!approvalsLayout || !approvalsContainer) {
        return;
    }

    while (QLayoutItem* item = approvalsLayout->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    approvalCards.clear();
    emptyApprovalsLabel = nullptr;

    const UiPalette u = ThemeManager::instance().ui();
    const QColor mutedCol = compositeOver(u.textMuted, u.background);
    const qsizetype count = pendingApprovals.size();

    if (pendingCountBadge) {
        pendingCountBadge->setText(QStringLiteral("%1 PENDING").arg(count));
        pendingCountBadge->setTone(count > 0 ? BadgeTone::Warning : BadgeTone::Neutral);
        pendingCountBadge->setShowDot(count > 0);
    }
    if (denyAllBtn) {
        denyAllBtn->setVisible(count > 1);
    }

    if (pendingApprovals.isEmpty()) {
        emptyApprovalsLabel = new QLabel(
            QStringLiteral("No pending terminal approval requests. When an agent invokes "
                           "type_in_terminal, the command appears here for explicit approval."),
            approvalsContainer);
        emptyApprovalsLabel->setObjectName(QStringLiteral("emptyApprovalsLabel"));
        emptyApprovalsLabel->setWordWrap(true);
        emptyApprovalsLabel->setStyleSheet(QStringLiteral(
            "QLabel { background: transparent; color: %1; font-size: 11.5px; }")
                                               .arg(qssColor(mutedCol)));
        approvalsLayout->addWidget(emptyApprovalsLabel);
        return;
    }

    for (const TerminalApprovalRequest& req : pendingApprovals) {
        auto* itemFrame = new QFrame(approvalsContainer);
        itemFrame->setObjectName(QStringLiteral("terminalApprovalCard"));
        itemFrame->setFrameShape(QFrame::NoFrame);
        itemFrame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

        auto* itemLayout = new QVBoxLayout(itemFrame);
        itemLayout->setContentsMargins(10, 9, 10, 9);
        itemLayout->setSpacing(7);

        // Top metadata row
        auto* metaRow = new QHBoxLayout();
        metaRow->setContentsMargins(0, 0, 0, 0);
        metaRow->setSpacing(8);

        auto* kindBadge = new SpellBadge(
            QStringLiteral("TYPE_IN_TERMINAL"), BadgeTone::Warning, true, itemFrame);

        QStringList metaParts;
        if (!req.threadId.trimmed().isEmpty()) {
            metaParts.append(QStringLiteral("Thread %1").arg(shortenIdentifier(req.threadId)));
        }
        const QString ts = formatTimestamp(req.createdAt);
        if (!ts.isEmpty()) {
            metaParts.append(ts);
        }
        if (metaParts.isEmpty() && !req.id.trimmed().isEmpty()) {
            metaParts.append(QStringLiteral("ID %1").arg(shortenIdentifier(req.id)));
        }
        auto* metaLabel = new QLabel(metaParts.join(QStringLiteral(" · ")), itemFrame);
        metaLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

        metaRow->addWidget(kindBadge);
        metaRow->addWidget(metaLabel);
        metaRow->addStretch(1);
        itemLayout->addLayout(metaRow);

        // Command text block
        const QString displayCmd = req.command.trimmed().isEmpty()
                                       ? QStringLiteral("$ (empty command)")
                                       : QStringLiteral("$ ") + req.command;
        auto* cmdLabel = new QLabel(displayCmd, itemFrame);
        cmdLabel->setObjectName(QStringLiteral("terminalApprovalCommandLabel"));
        cmdLabel->setTextFormat(Qt::PlainText);
        cmdLabel->setWordWrap(true);
        cmdLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        itemLayout->addWidget(cmdLabel);

        // Optional agent reason
        QLabel* reasonLabel = nullptr;
        if (!req.reason.trimmed().isEmpty()) {
            reasonLabel = new QLabel(
                QStringLiteral("Reason: ") + req.reason.trimmed(), itemFrame);
            reasonLabel->setTextFormat(Qt::PlainText);
            reasonLabel->setWordWrap(true);
            reasonLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            itemLayout->addWidget(reasonLabel);
        }

        // Approve / Deny buttons
        auto* btnRow = new QHBoxLayout();
        btnRow->setContentsMargins(0, 2, 0, 0);
        btnRow->setSpacing(8);

        const QString approvalId = req.id;

        auto* approveBtn = new QPushButton(QStringLiteral("Approve"), itemFrame);
        approveBtn->setObjectName(QStringLiteral("approveTerminalButton_") + approvalId);
        approveBtn->setCursor(Qt::PointingHandCursor);
        approveBtn->setIconSize(QSize(14, 14));
        approveBtn->setToolTip(
            QStringLiteral("Approve and run this command in the integrated terminal"));
        QObject::connect(approveBtn, &QPushButton::clicked, owner, [this, approvalId]() {
            resolveApproval(approvalId, true);
        });

        auto* denyBtn = new QPushButton(QStringLiteral("Deny"), itemFrame);
        denyBtn->setObjectName(QStringLiteral("denyTerminalButton_") + approvalId);
        denyBtn->setProperty("danger", true);
        denyBtn->setCursor(Qt::PointingHandCursor);
        denyBtn->setIconSize(QSize(14, 14));
        denyBtn->setToolTip(QStringLiteral("Deny this command without executing it"));
        QObject::connect(denyBtn, &QPushButton::clicked, owner, [this, approvalId]() {
            resolveApproval(approvalId, false);
        });

        btnRow->addWidget(approveBtn);
        btnRow->addWidget(denyBtn);
        btnRow->addStretch(1);
        itemLayout->addLayout(btnRow);

        ApprovalCardRefs refs{
            itemFrame,
            kindBadge,
            metaLabel,
            cmdLabel,
            reasonLabel,
            approveBtn,
            denyBtn
        };
        styleApprovalCard(refs, u);
        approvalCards.append(refs);
        approvalsLayout->addWidget(itemFrame);
    }
}

void TerminalSettingsPane::Impl::upsertPendingApproval(const TerminalApprovalRequest& req) {
    if (!req.id.isEmpty()) {
        recentlyResolvedIds.remove(req.id);
        for (TerminalApprovalRequest& existing : pendingApprovals) {
            if (existing.id == req.id) {
                if (existing == req) {
                    return;
                }
                existing = req;
                rebuildApprovalsList();
                return;
            }
        }
    }
    pendingApprovals.append(req);
    rebuildApprovalsList();
}

void TerminalSettingsPane::Impl::resolveApproval(const QString& approvalId, bool approved) {
    QString cmdSummary;
    auto it = std::find_if(pendingApprovals.begin(), pendingApprovals.end(),
                           [&approvalId](const TerminalApprovalRequest& r) {
                               return r.id == approvalId;
                           });
    if (it != pendingApprovals.end()) {
        cmdSummary = truncateForSummary(it->command);
        pendingApprovals.erase(it);
    }

    if (client) {
        if (!approvalId.isEmpty()) {
            recentlyResolvedIds.insert(approvalId, QDateTime::currentMSecsSinceEpoch());
        }
        client->respondTerminalApproval(approvalId, approved);
    }

    feedbackIsApproval = approved;
    if (feedbackLabel) {
        const QString prefix = approved ? QStringLiteral("Approved: ") : QStringLiteral("Denied: ");
        const QString fallback = approved ? QStringLiteral("Terminal command approved.")
                                          : QStringLiteral("Terminal command denied.");
        feedbackLabel->setText(cmdSummary.isEmpty() ? fallback : prefix + cmdSummary);
        applyFeedbackStyle(ThemeManager::instance().ui());
        feedbackLabel->setVisible(true);
    }

    rebuildApprovalsList();
}

void TerminalSettingsPane::Impl::denyAllPendingApprovals() {
    if (pendingApprovals.isEmpty()) {
        return;
    }

    const QVector<TerminalApprovalRequest> toDeny = pendingApprovals;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    pendingApprovals.clear();

    if (client) {
        for (const TerminalApprovalRequest& req : toDeny) {
            if (!req.id.isEmpty()) {
                recentlyResolvedIds.insert(req.id, nowMs);
            }
            client->respondTerminalApproval(req.id, false);
        }
    }

    feedbackIsApproval = false;
    if (feedbackLabel) {
        feedbackLabel->setText(
            QStringLiteral("Denied %1 pending terminal command(s).").arg(toDeny.size()));
        applyFeedbackStyle(ThemeManager::instance().ui());
        feedbackLabel->setVisible(true);
    }

    rebuildApprovalsList();
}

void TerminalSettingsPane::Impl::applyFeedbackStyle(const UiPalette& u) {
    if (!feedbackLabel) {
        return;
    }
    const QColor color = feedbackIsApproval ? u.diffAdded : u.danger;
    QFont mono = ThemeManager::instance().editorFont();
    mono.setPointSizeF(10.5);
    feedbackLabel->setFont(mono);
    feedbackLabel->setStyleSheet(QStringLiteral(
        "QLabel#terminalApprovalFeedbackLabel {"
        "  background: transparent;"
        "  color: %1;"
        "}")
                                     .arg(qssColor(color)));
}

// =============================================================================
// TerminalSettingsPane Public API
// =============================================================================

TerminalSettingsPane::TerminalSettingsPane(SpellCoreClient* client, QWidget* parent)
    : QWidget(parent)
    , m_impl(std::make_unique<Impl>()) {
    m_impl->owner = this;
    m_impl->client = client;

    m_impl->setupUi();
    m_impl->updateStateDisplay();
    m_impl->rebuildApprovalsList();
    applyTheme();

    connect(&ThemeManager::instance(), &ThemeManager::terminalThemeChanged, this, [this]() {
        m_impl->syncThemeControlsFromManager();
        m_impl->updateTerminalPreview();
    });

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        applyTheme();
    });

    if (m_impl->client) {
        connect(m_impl->client, &SpellCoreClient::terminalCommandRequested, this,
                [this](const TerminalApprovalRequest& req) {
                    m_impl->upsertPendingApproval(req);
                });
        updateSnapshot(m_impl->client->currentSnapshot());
    }
}

TerminalSettingsPane::~TerminalSettingsPane() = default;

void TerminalSettingsPane::updateSnapshot(const WorkspaceSnapshot& snap) {
    // 1. Update active workspace directory metadata
    QString activeRoot;
    for (const ProjectInfo& proj : snap.projects) {
        if (proj.id == snap.selectedProjectId) {
            activeRoot = proj.rootPath.trimmed();
            break;
        }
    }
    if (activeRoot.isEmpty() && !snap.projects.isEmpty()) {
        activeRoot = snap.projects.first().rootPath.trimmed();
    }
    if (m_impl->directoryRow) {
        m_impl->directoryRow->setValue(activeRoot.isEmpty() ? QStringLiteral("—") : activeRoot);
        m_impl->directoryRow->setToolTip(activeRoot);
    }

    // 2. Update agent terminal tool access indicators
    QString readLabel = QStringLiteral("Allowed");
    BadgeTone readTone = BadgeTone::Info;
    QString writeLabel = QStringLiteral("Approval every time");
    BadgeTone writeTone = BadgeTone::Warning;

    for (const ToolPermissionEntry& perm : snap.toolPermissions) {
        if (perm.toolName.compare(QLatin1StringView("read_terminal"), Qt::CaseInsensitive) == 0) {
            const QString mode = perm.mode.trimmed().toLower();
            if (mode == QLatin1StringView("deny")) {
                readLabel = QStringLiteral("Denied");
                readTone = BadgeTone::Danger;
            } else if (mode == QLatin1StringView("ask") || mode == QLatin1StringView("confirm")) {
                readLabel = QStringLiteral("Ask every time");
                readTone = BadgeTone::Warning;
            } else {
                readLabel = QStringLiteral("Allowed");
                readTone = BadgeTone::Info;
            }
        } else if (perm.toolName.compare(QLatin1StringView("type_in_terminal"), Qt::CaseInsensitive) == 0) {
            const QString mode = perm.mode.trimmed().toLower();
            if (mode == QLatin1StringView("deny")) {
                writeLabel = QStringLiteral("Denied");
                writeTone = BadgeTone::Danger;
            } else {
                writeLabel = QStringLiteral("Approval every time");
                writeTone = BadgeTone::Warning;
            }
        }
    }

    if (m_impl->readAccessRow) {
        m_impl->readAccessRow->setValue(readLabel);
        m_impl->readAccessRow->setTone(readTone);
    }
    if (m_impl->writeAccessRow) {
        m_impl->writeAccessRow->setValue(writeLabel);
        m_impl->writeAccessRow->setTone(writeTone);
    }

    // 3. Synchronize pending type_in_terminal approval requests
    if (!m_impl->recentlyResolvedIds.isEmpty()) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        for (auto it = m_impl->recentlyResolvedIds.begin(); it != m_impl->recentlyResolvedIds.end();) {
            const QString& resolvedId = it.key();
            const bool stillInSnap = std::any_of(
                snap.pendingTerminalApprovals.cbegin(), snap.pendingTerminalApprovals.cend(),
                [&resolvedId](const TerminalApprovalRequest& r) { return r.id == resolvedId; });
            if (!stillInSnap || (nowMs - it.value()) > kResolvedGraceMs) {
                it = m_impl->recentlyResolvedIds.erase(it);
            } else {
                ++it;
            }
        }
    }

    QVector<TerminalApprovalRequest> filtered;
    filtered.reserve(snap.pendingTerminalApprovals.size());
    for (const TerminalApprovalRequest& req : snap.pendingTerminalApprovals) {
        if (!req.id.isEmpty() && m_impl->recentlyResolvedIds.contains(req.id)) {
            continue;
        }
        filtered.append(req);
    }

    if (m_impl->pendingApprovals != filtered) {
        m_impl->pendingApprovals = std::move(filtered);
        m_impl->rebuildApprovalsList();
    }
}

void TerminalSettingsPane::setTerminalState(bool docked, bool poppedOut) {
    if (m_impl->docked == docked && m_impl->poppedOut == poppedOut) {
        return;
    }
    m_impl->docked = docked;
    m_impl->poppedOut = poppedOut;
    m_impl->updateStateDisplay();
}

void TerminalSettingsPane::applyTheme() {
    const auto& tm = ThemeManager::instance();
    const UiPalette u = tm.ui();

    const QColor cardBg = u.surface;
    const QColor borderCol = compositeOver(u.subtleBorder, u.background);
    const QColor secondaryCol = compositeOver(u.textSecondary, u.background);
    const QColor mutedCol = compositeOver(u.textMuted, u.background);

    const QString cardQss = QStringLiteral(
        "QFrame#spellSettingsCard {"
        "  background-color: %1;"
        "  border: 1px solid %2;"
        "  border-radius: 8px;"
        "}")
                                .arg(qssColor(cardBg), qssColor(borderCol));

    for (QFrame* card : m_impl->cards) {
        if (card) {
            card->setStyleSheet(cardQss);
        }
    }

    for (SpellSectionHeader* header : m_impl->sectionHeaders) {
        if (header) {
            header->applyTheme();
        }
    }

    for (SpellHairline* hairline : m_impl->hairlines) {
        if (hairline) {
            hairline->applyTheme();
        }
    }

    const QString captionQss = QStringLiteral(
        "QLabel { background: transparent; color: %1; font-size: 12px; }")
                                   .arg(qssColor(secondaryCol));
    for (QLabel* lbl : m_impl->captionLabels) {
        if (lbl) {
            lbl->setStyleSheet(captionQss);
        }
    }

    const QString mutedQss = QStringLiteral(
        "QLabel { background: transparent; color: %1; font-size: 11px; }")
                                 .arg(qssColor(mutedCol));
    for (QLabel* lbl : m_impl->mutedLabels) {
        if (lbl) {
            lbl->setStyleSheet(mutedQss);
        }
    }

    if (m_impl->approvalsHeaderLabel) {
        QFont labelFont = tm.editorFont();
        labelFont.setPointSizeF(9.5);
        labelFont.setBold(true);
        m_impl->approvalsHeaderLabel->setFont(labelFont);
        m_impl->approvalsHeaderLabel->setStyleSheet(QStringLiteral(
            "QLabel#pendingApprovalsHeaderLabel { background: transparent; color: %1; }")
                                                        .arg(qssColor(mutedCol)));
    }

    if (m_impl->emptyApprovalsLabel) {
        m_impl->emptyApprovalsLabel->setStyleSheet(QStringLiteral(
            "QLabel#emptyApprovalsLabel { background: transparent; color: %1; font-size: 11.5px; }")
                                                       .arg(qssColor(mutedCol)));
    }

    if (m_impl->statusBadge) {
        m_impl->statusBadge->applyTheme();
    }
    if (m_impl->pendingCountBadge) {
        m_impl->pendingCountBadge->applyTheme();
    }
    if (m_impl->placementRow) {
        m_impl->placementRow->applyTheme();
    }
    if (m_impl->directoryRow) {
        m_impl->directoryRow->applyTheme();
    }
    if (m_impl->readAccessRow) {
        m_impl->readAccessRow->applyTheme();
    }
    if (m_impl->writeAccessRow) {
        m_impl->writeAccessRow->applyTheme();
    }

    if (m_impl->toggleDockedBtn) {
        m_impl->toggleDockedBtn->setIcon(
            icons::vectorIcon(QStringLiteral("terminal"), u.textPrimary, 14));
        repolishWidget(m_impl->toggleDockedBtn);
    }
    if (m_impl->popOutBtn) {
        m_impl->popOutBtn->setIcon(
            icons::vectorIcon(QStringLiteral("popout"), u.textPrimary, 14));
        repolishWidget(m_impl->popOutBtn);
    }
    if (m_impl->clearBufferBtn) {
        m_impl->clearBufferBtn->setIcon(
            icons::vectorIcon(QStringLiteral("trash"), secondaryCol, 14));
        repolishWidget(m_impl->clearBufferBtn);
    }
    if (m_impl->restartShellBtn) {
        m_impl->restartShellBtn->setIcon(
            icons::vectorIcon(QStringLiteral("refresh"), u.textPrimary, 14));
        repolishWidget(m_impl->restartShellBtn);
    }
    if (m_impl->denyAllBtn) {
        const QColor dangerFill = withAlpha(u.danger, 0.14);
        const QColor dangerHover = withAlpha(u.danger, 0.24);
        const QColor dangerBorder = withAlpha(u.danger, 0.70);
        m_impl->denyAllBtn->setIcon(
            icons::vectorIcon(QStringLiteral("close"), u.danger, 13));
        m_impl->denyAllBtn->setStyleSheet(QStringLiteral(
            "QPushButton#denyAllTerminalApprovalsButton {"
            "  background-color: %1;"
            "  color: %2;"
            "  border: 1px solid %3;"
            "  border-radius: 6px;"
            "  padding: 3px 10px;"
            "  font-weight: 600;"
            "}"
            "QPushButton#denyAllTerminalApprovalsButton:hover {"
            "  background-color: %4;"
            "}")
                                              .arg(qssColor(dangerFill),
                                                   qssColor(u.danger),
                                                   qssColor(dangerBorder),
                                                   qssColor(dangerHover)));
    }

    if (m_impl->fontSizeSlider) {
        m_impl->fontSizeSlider->setStyleSheet(QStringLiteral(
            "QSlider {"
            "  background: transparent;"
            "  border: none;"
            "}"
            "QSlider::groove:horizontal {"
            "  height: 4px;"
            "  border-radius: 2px;"
            "  background: %1;"
            "}"
            "QSlider::sub-page:horizontal {"
            "  height: 4px;"
            "  border-radius: 2px;"
            "  background: %2;"
            "}"
            "QSlider::handle:horizontal {"
            "  width: 14px;"
            "  height: 14px;"
            "  margin: -5px 0;"
            "  border-radius: 7px;"
            "  background: %2;"
            "  border: 2px solid %3;"
            "}")
                                                  .arg(qssColor(borderCol),
                                                       qssColor(u.accent),
                                                       qssColor(u.background)));
    }

    m_impl->syncThemeControlsFromManager();
    m_impl->updateTerminalPreview();
    m_impl->applyFeedbackStyle(u);

    for (const auto& refs : m_impl->approvalCards) {
        m_impl->styleApprovalCard(refs, u);
    }
}

} // namespace spell
