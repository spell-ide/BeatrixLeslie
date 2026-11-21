#include "widgets/SpellDiffWidget.h"

#include "theme/SpellIcons.h"
#include "widgets/SpellBadges.h"

#include <QFont>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSizePolicy>
#include <QStringView>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFormat>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace spell {

namespace {

constexpr const char* kCardObjectName = "SpellDiffCard";
constexpr const char* kDiffViewObjectName = "SpellDiffView";
constexpr const char* kCollapseButtonObjectName = "SpellDiffCollapseButton";

struct StatusPresentation {
    QString label;
    BadgeTone tone = BadgeTone::Neutral;
    bool showDot = false;
};

StatusPresentation describeStatus(const QString& rawStatus) {
    const QString trimmed = rawStatus.trimmed();
    const QString lower = trimmed.toLower();

    if (lower.isEmpty()) {
        return {};
    }
    if (lower == QLatin1StringView("pending")) {
        return {QStringLiteral("Pending"), BadgeTone::Warning, true};
    }
    if (lower == QLatin1StringView("staged")) {
        return {QStringLiteral("Staged"), BadgeTone::Accent, true};
    }
    if (lower == QLatin1StringView("verified")) {
        return {QStringLiteral("Verified"), BadgeTone::Success, true};
    }
    if (lower == QLatin1StringView("approved")) {
        return {QStringLiteral("Approved"), BadgeTone::Success, false};
    }
    if (lower == QLatin1StringView("applied")) {
        return {QStringLiteral("Applied"), BadgeTone::Success, false};
    }
    if (lower == QLatin1StringView("committed")) {
        return {QStringLiteral("Committed"), BadgeTone::Success, false};
    }
    if (lower == QLatin1StringView("accepted")) {
        return {QStringLiteral("Accepted"), BadgeTone::Success, false};
    }
    if (lower == QLatin1StringView("rejected")) {
        return {QStringLiteral("Rejected"), BadgeTone::Danger, false};
    }
    if (lower == QLatin1StringView("discarded")) {
        return {QStringLiteral("Discarded"), BadgeTone::Danger, false};
    }
    if (lower == QLatin1StringView("rolled_back") || lower == QLatin1StringView("rolledback")) {
        return {QStringLiteral("Rolled Back"), BadgeTone::Danger, false};
    }
    if (lower == QLatin1StringView("reverted")) {
        return {QStringLiteral("Reverted"), BadgeTone::Danger, false};
    }
    if (lower == QLatin1StringView("failed") || lower == QLatin1StringView("error")) {
        return {lower == QLatin1StringView("failed") ? QStringLiteral("Failed") : QStringLiteral("Error"),
                BadgeTone::Danger,
                false};
    }

    QString formatted = trimmed;
    formatted.replace(u'_', u' ');
    if (!formatted.isEmpty()) {
        formatted[0] = formatted[0].toUpper();
    }
    return {formatted, BadgeTone::Neutral, false};
}

QString normalizeDiffText(const QString& rawDiff) {
    if (rawDiff.isEmpty()) {
        return {};
    }
    QString normalized;
    normalized.reserve(rawDiff.size());
    const qsizetype len = rawDiff.size();
    for (qsizetype i = 0; i < len; ++i) {
        const QChar ch = rawDiff[i];
        if (ch == u'\r') {
            normalized.append(u'\n');
            if ((i + 1) < len && rawDiff[i + 1] == u'\n') {
                ++i;
            }
        } else {
            normalized.append(ch);
        }
    }
    if (normalized.endsWith(u'\n')) {
        normalized.chop(1);
    }
    return normalized;
}

bool isDiffMetadataLine(const QString& line) {
    return line.startsWith(QLatin1StringView("+++"))
        || line.startsWith(QLatin1StringView("---"))
        || line.startsWith(QLatin1StringView("diff --git "))
        || line.startsWith(QLatin1StringView("index "))
        || line.startsWith(QLatin1StringView("new file mode"))
        || line.startsWith(QLatin1StringView("deleted file mode"))
        || line.startsWith(QLatin1StringView("old mode"))
        || line.startsWith(QLatin1StringView("new mode"))
        || line.startsWith(QLatin1StringView("similarity index"))
        || line.startsWith(QLatin1StringView("rename from "))
        || line.startsWith(QLatin1StringView("rename to "))
        || line.startsWith(QLatin1StringView("\\ No newline"));
}

QFont makeMonoFont(double pointSize, QFont::Weight weight) {
    QFont font(ThemeManager::instance().editorFontFamily());
    font.setStyleHint(QFont::Monospace);
    font.setPointSizeF(std::clamp(pointSize, 9.0, 24.0));
    font.setWeight(weight);
    return font;
}

} // namespace

SpellDiffWidget::SpellDiffWidget(QWidget* parent)
    : SpellDiffWidget(StagedDiff{}, true, parent) {}

SpellDiffWidget::SpellDiffWidget(const StagedDiff& diff, QWidget* parent)
    : SpellDiffWidget(diff, true, parent) {}

SpellDiffWidget::SpellDiffWidget(const StagedDiff& diff, bool showActions, QWidget* parent)
    : QFrame(parent),
      m_diff(diff),
      m_showActions(showActions) {
    setupUi();

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SpellDiffWidget::applyTheme);

    updateHeaderAndActions();
    applyTheme();
}

void SpellDiffWidget::setDiff(const StagedDiff& diff) {
    m_diff = diff;
    updateHeaderAndActions();
    renderUnifiedDiff();
}

void SpellDiffWidget::setShowActions(bool show) {
    if (m_showActions == show) {
        return;
    }
    m_showActions = show;
    updateHeaderAndActions();
}

void SpellDiffWidget::setCollapsed(bool collapsed) {
    if (m_collapsed == collapsed) {
        return;
    }
    m_collapsed = collapsed;
    updateHeaderAndActions();
    updateDiffViewHeight();
}

void SpellDiffWidget::setMaxDiffHeight(int maxHeightPx) {
    const int clamped = std::max(24, maxHeightPx);
    if (m_maxDiffHeight == clamped) {
        return;
    }
    m_maxDiffHeight = clamped;
    updateDiffViewHeight();
}

bool SpellDiffWidget::isPendingStatus(const QString& status) {
    const QString normalized = status.trimmed().toLower();
    return normalized == QLatin1StringView("pending")
        || normalized == QLatin1StringView("staged")
        || normalized == QLatin1StringView("verified");
}

void SpellDiffWidget::computeLineCounts(const QString& unifiedDiff,
                                        int* outAdded,
                                        int* outRemoved) {
    int added = 0;
    int removed = 0;

    const QStringView view(unifiedDiff);
    qsizetype pos = 0;
    const qsizetype len = view.size();
    while (pos < len) {
        qsizetype end = pos;
        while (end < len && view[end] != u'\n' && view[end] != u'\r') {
            ++end;
        }
        const QStringView line = view.sliced(pos, end - pos);
        if (line.startsWith(u'+') && !line.startsWith(u"+++")) {
            ++added;
        } else if (line.startsWith(u'-') && !line.startsWith(u"---")) {
            ++removed;
        }
        if (end < len && view[end] == u'\r' && (end + 1) < len && view[end + 1] == u'\n') {
            pos = end + 2;
        } else {
            pos = end + 1;
        }
    }

    if (outAdded) {
        *outAdded = added;
    }
    if (outRemoved) {
        *outRemoved = removed;
    }
}

void SpellDiffWidget::setupUi() {
    setObjectName(QString::fromLatin1(kCardObjectName));
    setFrameShape(QFrame::NoFrame);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(10, 8, 10, 8);
    rootLayout->setSpacing(6);

    auto* headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(6);

    auto* collapseButton = new QToolButton(this);
    collapseButton->setObjectName(QString::fromLatin1(kCollapseButtonObjectName));
    collapseButton->setAutoRaise(true);
    collapseButton->setCursor(Qt::PointingHandCursor);
    collapseButton->setFixedSize(20, 20);
    collapseButton->setIconSize(QSize(12, 12));
    collapseButton->setFocusPolicy(Qt::NoFocus);
    connect(collapseButton, &QToolButton::clicked, this, [this]() {
        setCollapsed(!m_collapsed);
    });
    headerLayout->addWidget(collapseButton);

    m_statusBadge = new SpellBadge(this);
    m_statusBadge->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    headerLayout->addWidget(m_statusBadge);

    m_fileButton = new QPushButton(this);
    m_fileButton->setFlat(true);
    m_fileButton->setCursor(Qt::PointingHandCursor);
    m_fileButton->setIconSize(QSize(13, 13));
    m_fileButton->setMinimumWidth(60);
    m_fileButton->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    connect(m_fileButton, &QPushButton::clicked, this, [this]() {
        if (!m_diff.filePath.isEmpty()) {
            emit openFileRequested(m_diff.filePath);
        }
    });
    headerLayout->addWidget(m_fileButton, 1);

    m_addedLabel = new QLabel(this);
    m_addedLabel->setAlignment(Qt::AlignCenter);
    m_addedLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    headerLayout->addWidget(m_addedLabel);

    m_removedLabel = new QLabel(this);
    m_removedLabel->setAlignment(Qt::AlignCenter);
    m_removedLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    headerLayout->addWidget(m_removedLabel);

    m_approveButton = new QPushButton(QStringLiteral("Approve"), this);
    m_approveButton->setCursor(Qt::PointingHandCursor);
    m_approveButton->setIconSize(QSize(12, 12));
    m_approveButton->setToolTip(QStringLiteral("Approve and apply staged diff"));
    m_approveButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(m_approveButton, &QPushButton::clicked, this, [this]() {
        emit approveClicked(m_diff.id);
    });
    headerLayout->addWidget(m_approveButton);

    m_rejectButton = new QPushButton(QStringLiteral("Reject"), this);
    m_rejectButton->setCursor(Qt::PointingHandCursor);
    m_rejectButton->setIconSize(QSize(12, 12));
    m_rejectButton->setToolTip(QStringLiteral("Reject and discard staged diff"));
    m_rejectButton->setProperty("danger", true);
    m_rejectButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(m_rejectButton, &QPushButton::clicked, this, [this]() {
        emit rejectClicked(m_diff.id);
    });
    headerLayout->addWidget(m_rejectButton);

    rootLayout->addLayout(headerLayout);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_summaryLabel->setVisible(false);
    rootLayout->addWidget(m_summaryLabel);

    m_diffView = new QPlainTextEdit(this);
    m_diffView->setObjectName(QString::fromLatin1(kDiffViewObjectName));
    m_diffView->setReadOnly(true);
    m_diffView->setUndoRedoEnabled(false);
    m_diffView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_diffView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_diffView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_diffView->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_diffView->setFocusPolicy(Qt::ClickFocus);
    m_diffView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_diffView->document()->setDocumentMargin(6.0);
    rootLayout->addWidget(m_diffView);
}

void SpellDiffWidget::updateHeaderAndActions() {
    const StatusPresentation statusInfo = describeStatus(m_diff.status);
    if (statusInfo.label.isEmpty()) {
        m_statusBadge->setText(QString());
        m_statusBadge->setVisible(false);
    } else {
        m_statusBadge->setText(statusInfo.label);
        m_statusBadge->setTone(statusInfo.tone);
        m_statusBadge->setShowDot(statusInfo.showDot);
        m_statusBadge->setVisible(true);
    }

    const QString path = m_diff.filePath.trimmed();
    if (path.isEmpty()) {
        m_fileButton->setText(QStringLiteral("Untitled diff"));
        m_fileButton->setToolTip(QString());
        m_fileButton->setEnabled(false);
        m_fileButton->setCursor(Qt::ArrowCursor);
    } else {
        m_fileButton->setText(path);
        m_fileButton->setToolTip(QStringLiteral("Open %1 in editor").arg(path));
        m_fileButton->setEnabled(true);
        m_fileButton->setCursor(Qt::PointingHandCursor);
    }

    int added = std::max(0, m_diff.linesAdded);
    int removed = std::max(0, m_diff.linesRemoved);
    if (added == 0 && removed == 0 && !m_diff.unifiedDiff.isEmpty()) {
        computeLineCounts(m_diff.unifiedDiff, &added, &removed);
    }
    m_addedLabel->setText(QStringLiteral("+%1").arg(added));
    m_addedLabel->setToolTip(QStringLiteral("%1 added %2")
                                 .arg(added)
                                 .arg(added == 1 ? QStringLiteral("line") : QStringLiteral("lines")));
    m_removedLabel->setText(QStringLiteral("-%1").arg(removed));
    m_removedLabel->setToolTip(QStringLiteral("%1 removed %2")
                                   .arg(removed)
                                   .arg(removed == 1 ? QStringLiteral("line") : QStringLiteral("lines")));

    const bool actionsVisible = m_showActions && isPendingStatus(m_diff.status);
    m_approveButton->setVisible(actionsVisible);
    m_rejectButton->setVisible(actionsVisible);

    const QString summary = m_diff.summary.trimmed();
    const bool showSummary = !summary.isEmpty() && summary != path;
    m_summaryLabel->setText(showSummary ? summary : QString());
    m_summaryLabel->setVisible(showSummary);

    if (auto* collapseButton = findChild<QToolButton*>(QString::fromLatin1(kCollapseButtonObjectName))) {
        const bool hasDiff = !m_diff.unifiedDiff.isEmpty();
        collapseButton->setVisible(hasDiff);
        collapseButton->setToolTip(m_collapsed ? QStringLiteral("Expand diff")
                                               : QStringLiteral("Collapse diff"));
        const UiPalette u = ThemeManager::instance().ui();
        const QColor iconColor = compositeOver(u.textMuted, u.background);
        collapseButton->setIcon(icons::vectorIcon(
            m_collapsed ? QStringLiteral("chevron-right") : QStringLiteral("chevron-down"),
            iconColor,
            12));
    }
}

void SpellDiffWidget::applyTheme() {
    const ThemeManager& tm = ThemeManager::instance();
    const UiPalette u = tm.ui();
    const CodePalette code = tm.code();

    const QColor solidBorder = compositeOver(u.subtleBorder, u.background);
    const QColor solidDivider = compositeOver(u.divider, u.background);
    const QColor solidElevated = compositeOver(u.elevatedSurface, u.background);
    const QColor solidSelected = compositeOver(u.selectedFill, u.background);
    const QColor solidSecondary = compositeOver(u.textSecondary, u.background);
    const QColor solidMuted = compositeOver(u.textMuted, u.background);

    setStyleSheet(QStringLiteral(
        "QFrame#%1 {\n"
        "  background-color: %2;\n"
        "  border: 1px solid %3;\n"
        "  border-radius: 8px;\n"
        "}\n")
        .arg(QString::fromLatin1(kCardObjectName),
             qssColor(u.surface),
             qssColor(solidBorder)));

    if (auto* collapseButton = findChild<QToolButton*>(QString::fromLatin1(kCollapseButtonObjectName))) {
        collapseButton->setStyleSheet(QStringLiteral(
            "QToolButton {\n"
            "  background-color: transparent;\n"
            "  border: none;\n"
            "  border-radius: 4px;\n"
            "  padding: 2px;\n"
            "}\n"
            "QToolButton:hover {\n"
            "  background-color: %1;\n"
            "}\n")
            .arg(qssColor(solidElevated)));
        collapseButton->setIcon(icons::vectorIcon(
            m_collapsed ? QStringLiteral("chevron-right") : QStringLiteral("chevron-down"),
            solidMuted,
            12));
    }

    if (m_statusBadge) {
        m_statusBadge->applyTheme();
    }

    const double basePt = tm.editorFontSize();
    m_fileButton->setFont(makeMonoFont(basePt - 1.5, QFont::DemiBold));
    m_fileButton->setIcon(icons::vectorIcon(QStringLiteral("file"), solidSecondary, 13));
    m_fileButton->setStyleSheet(QStringLiteral(
        "QPushButton {\n"
        "  background-color: transparent;\n"
        "  color: %1;\n"
        "  border: none;\n"
        "  padding: 2px 4px;\n"
        "  text-align: left;\n"
        "  font-weight: 600;\n"
        "}\n"
        "QPushButton:hover {\n"
        "  background-color: transparent;\n"
        "  color: %1;\n"
        "  text-decoration: underline;\n"
        "}\n"
        "QPushButton:disabled {\n"
        "  color: %2;\n"
        "}\n")
        .arg(qssColor(u.textPrimary), qssColor(solidMuted)));

    const QFont pillFont = makeMonoFont(basePt - 2.5, QFont::DemiBold);
    m_addedLabel->setFont(pillFont);
    m_addedLabel->setStyleSheet(QStringLiteral(
        "QLabel {\n"
        "  background-color: %1;\n"
        "  color: %2;\n"
        "  border: 1px solid %3;\n"
        "  border-radius: 9px;\n"
        "  padding: 1px 6px;\n"
        "  font-weight: 600;\n"
        "}\n")
        .arg(qssColor(withAlpha(u.diffAdded, 0.14)),
             qssColor(u.diffAdded),
             qssColor(withAlpha(u.diffAdded, 0.36))));

    m_removedLabel->setFont(pillFont);
    m_removedLabel->setStyleSheet(QStringLiteral(
        "QLabel {\n"
        "  background-color: %1;\n"
        "  color: %2;\n"
        "  border: 1px solid %3;\n"
        "  border-radius: 9px;\n"
        "  padding: 1px 6px;\n"
        "  font-weight: 600;\n"
        "}\n")
        .arg(qssColor(withAlpha(u.diffRemoved, 0.14)),
             qssColor(u.diffRemoved),
             qssColor(withAlpha(u.diffRemoved, 0.36))));

    m_approveButton->setIcon(icons::vectorIcon(QStringLiteral("check"), u.diffAdded, 12));
    m_approveButton->setStyleSheet(QStringLiteral(
        "QPushButton {\n"
        "  background-color: %1;\n"
        "  color: %2;\n"
        "  border: 1px solid %3;\n"
        "  border-radius: 5px;\n"
        "  padding: 3px 9px;\n"
        "  font-size: 11px;\n"
        "  font-weight: 600;\n"
        "}\n"
        "QPushButton:hover {\n"
        "  background-color: %4;\n"
        "  border-color: %2;\n"
        "}\n"
        "QPushButton:pressed {\n"
        "  background-color: %5;\n"
        "}\n"
        "QPushButton:disabled {\n"
        "  color: %6;\n"
        "  border-color: %7;\n"
        "  background-color: transparent;\n"
        "}\n")
        .arg(qssColor(withAlpha(u.diffAdded, 0.15)),
             qssColor(u.diffAdded),
             qssColor(withAlpha(u.diffAdded, 0.55)),
             qssColor(withAlpha(u.diffAdded, 0.26)),
             qssColor(withAlpha(u.diffAdded, 0.34)),
             qssColor(solidMuted),
             qssColor(solidDivider)));

    m_rejectButton->setIcon(icons::vectorIcon(QStringLiteral("close"), u.diffRemoved, 12));
    m_rejectButton->setStyleSheet(QStringLiteral(
        "QPushButton {\n"
        "  background-color: %1;\n"
        "  color: %2;\n"
        "  border: 1px solid %3;\n"
        "  border-radius: 5px;\n"
        "  padding: 3px 9px;\n"
        "  font-size: 11px;\n"
        "  font-weight: 600;\n"
        "}\n"
        "QPushButton:hover {\n"
        "  background-color: %4;\n"
        "  border-color: %2;\n"
        "}\n"
        "QPushButton:pressed {\n"
        "  background-color: %5;\n"
        "}\n"
        "QPushButton:disabled {\n"
        "  color: %6;\n"
        "  border-color: %7;\n"
        "  background-color: transparent;\n"
        "}\n")
        .arg(qssColor(withAlpha(u.diffRemoved, 0.14)),
             qssColor(u.diffRemoved),
             qssColor(withAlpha(u.diffRemoved, 0.55)),
             qssColor(withAlpha(u.diffRemoved, 0.25)),
             qssColor(withAlpha(u.diffRemoved, 0.34)),
             qssColor(solidMuted),
             qssColor(solidDivider)));

    m_summaryLabel->setStyleSheet(QStringLiteral(
        "QLabel {\n"
        "  background-color: transparent;\n"
        "  color: %1;\n"
        "  font-size: 11px;\n"
        "  padding-left: 2px;\n"
        "}\n")
        .arg(qssColor(solidSecondary)));

    const QFont diffFont = makeMonoFont(basePt - 1.5, QFont::Normal);
    m_diffView->setFont(diffFont);
    m_diffView->setTabStopDistance(QFontMetricsF(diffFont).horizontalAdvance(u' ') * 4.0);

    const QColor codeBg = code.background;
    const QColor codeSelection = codeBg.lightness() < 128
        ? compositeOver(withAlpha(rgbHex(0xFFFFFF), 0.16), codeBg)
        : solidSelected;

    m_diffView->setStyleSheet(QStringLiteral(
        "QPlainTextEdit#%1 {\n"
        "  background-color: %2;\n"
        "  color: %3;\n"
        "  border: 1px solid %4;\n"
        "  border-radius: 6px;\n"
        "  padding: 2px 0px;\n"
        "  selection-background-color: %5;\n"
        "  selection-color: %3;\n"
        "}\n"
        "QPlainTextEdit#%1:focus {\n"
        "  border: 1px solid %6;\n"
        "}\n")
        .arg(QString::fromLatin1(kDiffViewObjectName),
             qssColor(codeBg),
             qssColor(code.plain),
             qssColor(solidDivider),
             qssColor(codeSelection),
             qssColor(solidBorder)));

    renderUnifiedDiff();
}

void SpellDiffWidget::renderUnifiedDiff() {
    if (!m_diffView) {
        return;
    }

    if (m_diff.unifiedDiff.isEmpty()) {
        m_diffView->clear();
        m_diffView->setExtraSelections({});
        updateDiffViewHeight();
        return;
    }

    const QString normalized = normalizeDiffText(m_diff.unifiedDiff);
    const bool sameText = (m_diffView->toPlainText() == normalized);
    const int savedVScroll = sameText ? m_diffView->verticalScrollBar()->value() : 0;
    const int savedHScroll = sameText ? m_diffView->horizontalScrollBar()->value() : 0;

    if (!sameText) {
        m_diffView->setPlainText(normalized);
    }

    const ThemeManager& tm = ThemeManager::instance();
    const CodePalette code = tm.code();
    const bool darkCodeSurface = code.background.lightness() < 128;

    const QColor addedFg = darkCodeSurface ? rgbHex(0x3FB950) : rgbHex(0x1A7F37);
    const QColor removedFg = darkCodeSurface ? rgbHex(0xF85149) : rgbHex(0xCF222E);
    const QColor addedBg = withAlpha(rgbHex(0x3FB950), darkCodeSurface ? 0.16 : 0.15);
    const QColor removedBg = withAlpha(rgbHex(0xF85149), darkCodeSurface ? 0.16 : 0.14);
    const QColor hunkFg = code.hunk;
    const QColor hunkBg = withAlpha(code.hunk, darkCodeSurface ? 0.12 : 0.10);
    const QColor metaFg = code.comment;
    const QColor plainFg = code.plain;

    QTextDocument* doc = m_diffView->document();
    QList<QTextEdit::ExtraSelection> extraSelections;
    extraSelections.reserve(doc->blockCount());

    QTextCursor cursor(doc);
    cursor.beginEditBlock();

    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        const QString line = block.text();
        const bool isAdd = line.startsWith(u'+') && !line.startsWith(QLatin1StringView("+++"));
        const bool isDel = line.startsWith(u'-') && !line.startsWith(QLatin1StringView("---"));
        const bool isHunk = line.startsWith(QLatin1StringView("@@"));
        const bool isMeta = !isAdd && !isDel && !isHunk && isDiffMetadataLine(line);

        QTextCharFormat charFmt;
        QColor rowTint;

        if (isAdd) {
            charFmt.setForeground(addedFg);
            rowTint = addedBg;
        } else if (isDel) {
            charFmt.setForeground(removedFg);
            rowTint = removedBg;
        } else if (isHunk) {
            charFmt.setForeground(hunkFg);
            charFmt.setFontWeight(QFont::DemiBold);
            rowTint = hunkBg;
        } else if (isMeta) {
            charFmt.setForeground(metaFg);
            if (line.startsWith(QLatin1StringView("\\ No newline"))) {
                charFmt.setFontItalic(true);
            } else {
                charFmt.setFontWeight(QFont::Medium);
            }
        } else {
            charFmt.setForeground(plainFg);
        }

        const int textLen = block.length() - 1;
        if (textLen > 0) {
            cursor.setPosition(block.position());
            cursor.setPosition(block.position() + textLen, QTextCursor::KeepAnchor);
            cursor.setCharFormat(charFmt);
        }

        if (rowTint.isValid()) {
            QTextEdit::ExtraSelection sel;
            sel.cursor = QTextCursor(block);
            sel.cursor.clearSelection();
            sel.format.setBackground(rowTint);
            sel.format.setProperty(QTextFormat::FullWidthSelection, true);
            extraSelections.append(sel);
        }
    }

    cursor.endEditBlock();
    m_diffView->setExtraSelections(extraSelections);

    if (sameText) {
        m_diffView->verticalScrollBar()->setValue(savedVScroll);
        m_diffView->horizontalScrollBar()->setValue(savedHScroll);
    } else {
        QTextCursor topCursor(doc);
        topCursor.movePosition(QTextCursor::Start);
        m_diffView->setTextCursor(topCursor);
        m_diffView->verticalScrollBar()->setValue(0);
        m_diffView->horizontalScrollBar()->setValue(0);
    }

    updateDiffViewHeight();
}

void SpellDiffWidget::updateDiffViewHeight() {
    if (!m_diffView) {
        return;
    }

    const bool shouldShow = !m_collapsed && !m_diff.unifiedDiff.isEmpty();
    m_diffView->setVisible(shouldShow);
    if (!shouldShow) {
        updateGeometry();
        return;
    }

    const int lineCount = std::max(1, m_diffView->document()->blockCount());
    const QFontMetrics fm(m_diffView->font());
    const int lineSpacing = std::max(1, fm.lineSpacing());
    const int docMargin = static_cast<int>(std::ceil(m_diffView->document()->documentMargin() * 2.0));
    const int frameHeight = m_diffView->frameWidth() * 2;
    const QMargins cm = m_diffView->contentsMargins();

    // Include small vertical padding + room for optional horizontal scrollbar.
    const int verticalOverhead = docMargin + frameHeight + cm.top() + cm.bottom() + 10;
    const int idealHeight = lineCount * lineSpacing + verticalOverhead;
    const int minHeight = std::min(m_maxDiffHeight, lineSpacing + verticalOverhead);
    const int targetHeight = std::clamp(idealHeight, std::max(1, minHeight), m_maxDiffHeight);

    m_diffView->setFixedHeight(targetHeight);
    updateGeometry();
}

} // namespace spell
