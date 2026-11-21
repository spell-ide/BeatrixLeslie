#include "panes/ComposerWidget.h"

#include "theme/SpellIcons.h"
#include "theme/SpellTheme.h"

#include <QAbstractTextDocumentLayout>
#include <QColor>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMetaMethod>
#include <QMimeData>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSplitter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <QTextOption>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QtGlobal>

#include <algorithm>

namespace spell {

namespace {

constexpr int kOuterMargin = 12;
constexpr int kRootSpacing = 8;
constexpr int kAttachmentsStripHeight = 26;
constexpr int kButtonWidth = 28;
constexpr int kButtonHeight = 26;
constexpr int kButtonIconPx = 12;

constexpr int kEditorLeftMargin = 7;
constexpr int kEditorTopMargin = 6;
constexpr int kEditorRightMargin = 7;
constexpr int kEditorBottomMargin = 2;

constexpr int kCompactMinEditorHeight = 44;
constexpr int kCompactMaxEditorHeight = 110;
constexpr int kExpandedEditorHeight = 220;

// Non-editor vertical chrome when no attachment pills are visible:
// outer margins (24) + card border (2) + editor margins (8) + button strip (26 + 6) = 66px.
constexpr int kBaseChromeHeight =
    (kOuterMargin * 2) + 2 + kEditorTopMargin + kEditorBottomMargin + kButtonHeight + 6;
constexpr int kAttachmentsExtraHeight = kAttachmentsStripHeight + kRootSpacing;

enum class PrimaryButtonVisual {
    Unset,
    SendEnabled,
    SendDisabled,
    Stop
};

QString sendShortcutTooltip() {
#ifdef Q_OS_MACOS
    return QStringLiteral("Send (\u2318\u21A9)");
#else
    return QStringLiteral("Send (Ctrl+Enter)");
#endif
}

QIcon makeProminentIcon(const QString& name, const QColor& color, int logicalSize) {
    const QIcon source = icons::vectorIcon(name, color, logicalSize);
    const QSize sz(logicalSize, logicalSize);
    QIcon out;
    out.addPixmap(source.pixmap(sz, 1.0, QIcon::Normal), QIcon::Normal);
    out.addPixmap(source.pixmap(sz, 2.0, QIcon::Normal), QIcon::Normal);
    out.addPixmap(source.pixmap(sz, 1.0, QIcon::Normal), QIcon::Disabled);
    out.addPixmap(source.pixmap(sz, 2.0, QIcon::Normal), QIcon::Disabled);
    return out;
}

bool hasAttachableLocalFiles(const QMimeData* mime) {
    if (!mime || !mime->hasUrls()) {
        return false;
    }
    const QList<QUrl> urls = mime->urls();
    for (const QUrl& url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QString localPath = url.toLocalFile().trimmed();
        if (!localPath.isEmpty() && !QFileInfo(localPath).isDir()) {
            return true;
        }
    }
    return false;
}

QStringList extractAttachableLocalFiles(const QMimeData* mime) {
    QStringList paths;
    if (!mime || !mime->hasUrls()) {
        return paths;
    }
    const QList<QUrl> urls = mime->urls();
    paths.reserve(urls.size());
    for (const QUrl& url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QString localPath = url.toLocalFile().trimmed();
        if (!localPath.isEmpty() && !QFileInfo(localPath).isDir()) {
            paths.append(localPath);
        }
    }
    return paths;
}

} // namespace

struct ComposerWidget::Impl {
    ComposerWidget* owner = nullptr;

    // Optional attached-files pill strip (hidden when empty)
    QScrollArea* attachmentsScroll = nullptr;
    QWidget* attachmentsRow = nullptr;
    QHBoxLayout* attachmentsLayout = nullptr;

    // Single rounded monochrome composer card
    QFrame* composerCard = nullptr;
    QPlainTextEdit* editor = nullptr;
    QPushButton* expandButton = nullptr;
    QPushButton* popOutTerminalButton = nullptr;
    QPushButton* sendStopButton = nullptr;

    // Active UI state
    bool busy = false;
    bool expanded = false;
    bool editorFocused = false;
    QString threadProfile = QStringLiteral("write");
    PrimaryButtonVisual lastPrimaryVisual = PrimaryButtonVisual::Unset;

    // Track auto-grow height changes in compact mode so manual splitter drags are respected
    int preAutoGrowHeight = 0;
    int autoGrownCompactHeight = 0;

    // Retained compatibility state (no stray controls rendered around composer)
    bool endpointConfigured = false;
    bool endpointOnline = false;
    QString modelId;
    bool stageWriteMode = false;
    bool terminalVisible = false;
    QVector<RubricTag> availableTags;
    QStringList activeTagIds;
    QStringList attachedFiles;

    [[nodiscard]] bool hasSendableContent() const {
        if (editor && !editor->toPlainText().trimmed().isEmpty()) {
            return true;
        }
        return !attachedFiles.isEmpty();
    }

    [[nodiscard]] bool canSend() const {
        return !busy && hasSendableContent();
    }

    [[nodiscard]] int chromeHeight() const {
        return kBaseChromeHeight + (attachedFiles.isEmpty() ? 0 : kAttachmentsExtraHeight);
    }

    [[nodiscard]] int compactEditorContentHeight() const {
        if (!editor) {
            return kCompactMinEditorHeight;
        }
        const QTextDocument* doc = editor->document();
        int visualLines = 0;
        if (doc) {
            for (QTextBlock blk = doc->begin(); blk.isValid(); blk = blk.next()) {
                if (!blk.isVisible()) {
                    continue;
                }
                const int blkLines = blk.layout()
                    ? std::max(1, blk.layout()->lineCount())
                    : std::max(1, blk.lineCount());
                visualLines += blkLines;
            }
        }
        visualLines = std::max(1, visualLines);

        const int lineHeight = std::max(16, editor->fontMetrics().lineSpacing());
        const int docMargin = doc ? qRound(doc->documentMargin() * 2.0) : 8;
        const int contentPx = (visualLines * lineHeight) + docMargin + 6;
        return std::clamp(contentPx, kCompactMinEditorHeight, kCompactMaxEditorHeight);
    }

    [[nodiscard]] int desiredEditorHeight() const {
        return expanded ? kExpandedEditorHeight : compactEditorContentHeight();
    }

    [[nodiscard]] int desiredTotalHeight() const {
        return chromeHeight() + desiredEditorHeight();
    }

    void adjustParentSplitterHeight(int targetHeight) {
        if (!owner) {
            return;
        }
        auto* splitter = qobject_cast<QSplitter*>(owner->parentWidget());
        if (!splitter || splitter->orientation() != Qt::Vertical) {
            return;
        }
        const int myIdx = splitter->indexOf(owner);
        if (myIdx < 0) {
            return;
        }
        QList<int> sizes = splitter->sizes();
        if (myIdx >= sizes.size() || sizes.size() < 2) {
            return;
        }

        int totalH = 0;
        for (int s : sizes) {
            totalH += s;
        }
        if (totalH <= 0) {
            return;
        }

        const int minTarget = std::max(owner->minimumHeight(), owner->minimumSizeHint().height());
        const int maxTarget = std::max(minTarget, totalH - 120);
        const int clampedTarget = std::clamp(targetHeight, minTarget, maxTarget);

        const int currentH = sizes.at(myIdx);
        const int delta = clampedTarget - currentH;
        if (delta == 0) {
            return;
        }

        // Prefer taking/giving space from index 0 (chat timeline) or the first visible sibling.
        int donorIdx = (myIdx != 0 && sizes.at(0) > 0) ? 0 : -1;
        if (donorIdx < 0) {
            for (int i = 0; i < sizes.size(); ++i) {
                if (i != myIdx && sizes.at(i) > 0) {
                    donorIdx = i;
                    break;
                }
            }
        }
        if (donorIdx < 0) {
            return;
        }

        constexpr int kMinSiblingHeight = 120;
        int appliedDelta = delta;
        if (appliedDelta > 0) {
            const int available = std::max(0, sizes.at(donorIdx) - kMinSiblingHeight);
            appliedDelta = std::min(appliedDelta, available);
        }
        if (appliedDelta == 0) {
            return;
        }

        sizes[myIdx] = currentH + appliedDelta;
        sizes[donorIdx] = sizes.at(donorIdx) - appliedDelta;
        splitter->setSizes(sizes);
    }

    void syncCompactAutoGrow() {
        if (!owner) {
            return;
        }
        owner->updateGeometry();

        if (expanded) {
            return;
        }
        auto* splitter = qobject_cast<QSplitter*>(owner->parentWidget());
        if (!splitter || splitter->orientation() != Qt::Vertical) {
            return;
        }
        const int myIdx = splitter->indexOf(owner);
        if (myIdx < 0) {
            return;
        }
        const QList<int> sizes = splitter->sizes();
        if (myIdx >= sizes.size()) {
            return;
        }
        const int currentH = sizes.at(myIdx);
        if (currentH <= 0) {
            return;
        }

        const int desired = desiredTotalHeight();
        if (currentH < desired) {
            if (autoGrownCompactHeight == 0) {
                preAutoGrowHeight = currentH;
            }
            adjustParentSplitterHeight(desired);
            const QList<int> updated = splitter->sizes();
            if (myIdx < updated.size()) {
                autoGrownCompactHeight = updated.at(myIdx);
            }
        } else if (autoGrownCompactHeight > 0 && currentH == autoGrownCompactHeight && desired < currentH) {
            const int target = std::max(preAutoGrowHeight, desired);
            adjustParentSplitterHeight(target);
            const QList<int> updated = splitter->sizes();
            if (myIdx < updated.size()) {
                const int newH = updated.at(myIdx);
                if (newH <= preAutoGrowHeight) {
                    autoGrownCompactHeight = 0;
                    preAutoGrowHeight = 0;
                } else {
                    autoGrownCompactHeight = newH;
                }
            }
        } else if (autoGrownCompactHeight > 0 && currentH != autoGrownCompactHeight) {
            // User manually resized the splitter; stop overriding their chosen height.
            autoGrownCompactHeight = 0;
            preAutoGrowHeight = 0;
        }
    }

    void updateComposerCardStyle() {
        if (!composerCard) {
            return;
        }
        const UiPalette u = ThemeManager::instance().ui();
        const QColor borderColor = editorFocused
            ? compositeOver(u.selectedBorder, u.background)
            : compositeOver(u.subtleBorder, u.background);

        composerCard->setStyleSheet(QStringLiteral(
            "QFrame#composerCard {"
            "  background-color: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 8px;"
            "}"
        ).arg(qssColor(u.background), qssColor(borderColor)));
    }

    QString outlinedIconButtonStyle(const QString& objectName) const {
        const UiPalette u = ThemeManager::instance().ui();
        const QColor solidBorder = compositeOver(u.subtleBorder, u.background);
        const QColor solidElevated = compositeOver(u.elevatedSurface, u.background);
        const QColor solidSelected = compositeOver(u.selectedFill, u.background);

        return QStringLiteral(
            "QPushButton#%1 {"
            "  background-color: transparent;"
            "  border: 1px solid %2;"
            "  border-radius: 6px;"
            "  padding: 0px;"
            "}"
            "QPushButton#%1:hover {"
            "  background-color: %3;"
            "  border: 1px solid %2;"
            "}"
            "QPushButton#%1:pressed {"
            "  background-color: %4;"
            "  border: 1px solid %2;"
            "}"
        ).arg(objectName, qssColor(solidBorder), qssColor(solidElevated), qssColor(solidSelected));
    }

    void updateExpandButtonVisual() {
        if (!expandButton) {
            return;
        }
        const UiPalette u = ThemeManager::instance().ui();
        const QString iconName = expanded ? QStringLiteral("collapse") : QStringLiteral("expand");
        expandButton->setIcon(icons::vectorIcon(iconName, u.textPrimary, kButtonIconPx));
        expandButton->setIconSize(QSize(kButtonIconPx, kButtonIconPx));
        expandButton->setToolTip(expanded
                                     ? QStringLiteral("Collapse the message box")
                                     : QStringLiteral("Expand the message box"));
        expandButton->setAccessibleName(expandButton->toolTip());
        expandButton->setStyleSheet(outlinedIconButtonStyle(QStringLiteral("composerExpandButton")));
    }

    void updatePopOutTerminalButtonVisual() {
        if (!popOutTerminalButton) {
            return;
        }
        const UiPalette u = ThemeManager::instance().ui();
        popOutTerminalButton->setIcon(
            icons::vectorIcon(QStringLiteral("terminal-popout"), u.textPrimary, kButtonIconPx));
        popOutTerminalButton->setIconSize(QSize(kButtonIconPx, kButtonIconPx));
        popOutTerminalButton->setToolTip(QStringLiteral("Pop out the message terminal"));
        popOutTerminalButton->setAccessibleName(popOutTerminalButton->toolTip());
        popOutTerminalButton->setStyleSheet(
            outlinedIconButtonStyle(QStringLiteral("composerPopOutTerminalButton")));
    }

    void updateProfileButtonVisual() {
        if (!editor) {
            return;
        }
        const bool isAsk = (threadProfile == QLatin1StringView("ask"));
        editor->setPlaceholderText(isAsk
            ? QStringLiteral("Ask BeatrixLeslie (read-only)\u2026")
            : QStringLiteral("Message BeatrixLeslie\u2026"));
    }

    void updateSendStopButton(bool forceRestyle = false) {
        if (!sendStopButton) {
            return;
        }

        const bool readyToSend = canSend();
        const PrimaryButtonVisual targetVisual = busy
            ? PrimaryButtonVisual::Stop
            : (readyToSend ? PrimaryButtonVisual::SendEnabled : PrimaryButtonVisual::SendDisabled);

        if (busy) {
            sendStopButton->setEnabled(true);
            sendStopButton->setToolTip(QStringLiteral("Stop"));
            sendStopButton->setAccessibleName(QStringLiteral("Stop"));
        } else {
            sendStopButton->setEnabled(readyToSend);
            sendStopButton->setToolTip(sendShortcutTooltip());
            sendStopButton->setAccessibleName(QStringLiteral("Send"));
        }

        if (!forceRestyle && targetVisual == lastPrimaryVisual) {
            return;
        }
        lastPrimaryVisual = targetVisual;

        const UiPalette u = ThemeManager::instance().ui();

        if (targetVisual == PrimaryButtonVisual::Stop) {
            const QColor hoverBg = compositeOver(withAlpha(u.textPrimary, 0.86), u.background);
            const QColor pressedBg = compositeOver(withAlpha(u.textPrimary, 0.74), u.background);

            sendStopButton->setIcon(makeProminentIcon(QStringLiteral("stop"), u.background, kButtonIconPx));
            sendStopButton->setIconSize(QSize(kButtonIconPx, kButtonIconPx));
            sendStopButton->setStyleSheet(QStringLiteral(
                "QPushButton#composerSendStopButton {"
                "  background-color: %1;"
                "  border: none;"
                "  border-radius: 6px;"
                "  padding: 0px;"
                "}"
                "QPushButton#composerSendStopButton:hover {"
                "  background-color: %2;"
                "}"
                "QPushButton#composerSendStopButton:pressed {"
                "  background-color: %3;"
                "}"
            ).arg(qssColor(u.textPrimary), qssColor(hoverBg), qssColor(pressedBg)));
        } else if (targetVisual == PrimaryButtonVisual::SendEnabled) {
            const QColor hoverBg = compositeOver(withAlpha(u.textPrimary, 0.86), u.background);
            const QColor pressedBg = compositeOver(withAlpha(u.textPrimary, 0.74), u.background);

            sendStopButton->setIcon(
                makeProminentIcon(QStringLiteral("arrow-up"), u.background, kButtonIconPx));
            sendStopButton->setIconSize(QSize(kButtonIconPx, kButtonIconPx));
            sendStopButton->setStyleSheet(QStringLiteral(
                "QPushButton#composerSendStopButton {"
                "  background-color: %1;"
                "  border: none;"
                "  border-radius: 6px;"
                "  padding: 0px;"
                "}"
                "QPushButton#composerSendStopButton:hover {"
                "  background-color: %2;"
                "}"
                "QPushButton#composerSendStopButton:pressed {"
                "  background-color: %3;"
                "}"
            ).arg(qssColor(u.textPrimary), qssColor(hoverBg), qssColor(pressedBg)));
        } else {
            // Disabled at exact 0.4 opacity over pure ui.background (monochrome inverted)
            const QColor disabledFill = compositeOver(withAlpha(u.textPrimary, 0.40), u.background);

            sendStopButton->setIcon(
                makeProminentIcon(QStringLiteral("arrow-up"), u.background, kButtonIconPx));
            sendStopButton->setIconSize(QSize(kButtonIconPx, kButtonIconPx));
            sendStopButton->setStyleSheet(QStringLiteral(
                "QPushButton#composerSendStopButton,"
                "QPushButton#composerSendStopButton:disabled {"
                "  background-color: %1;"
                "  border: none;"
                "  border-radius: 6px;"
                "  padding: 0px;"
                "}"
            ).arg(qssColor(disabledFill)));
        }
    }

    void rebuildAttachmentPills() {
        if (!attachmentsLayout || !attachmentsScroll || !owner) {
            return;
        }

        while (attachmentsLayout->count() > 0) {
            QLayoutItem* item = attachmentsLayout->takeAt(0);
            if (!item) {
                continue;
            }
            if (QWidget* w = item->widget()) {
                w->hide();
                w->setParent(nullptr);
                w->deleteLater();
            }
            delete item;
        }

        if (attachedFiles.isEmpty()) {
            attachmentsScroll->setVisible(false);
            attachmentsLayout->addStretch(1);
            syncCompactAutoGrow();
            return;
        }

        const UiPalette u = ThemeManager::instance().ui();
        const QColor solidBorder = compositeOver(u.subtleBorder, u.background);
        const QColor solidElevated = compositeOver(u.elevatedSurface, u.background);
        const QColor solidSecondary = compositeOver(u.textSecondary, u.background);
        const QColor solidMuted = compositeOver(u.textMuted, u.background);

        QFont pillFont = ThemeManager::instance().editorFont();
        pillFont.setPointSizeF(10.0);
        pillFont.setWeight(QFont::DemiBold);

        const QString pillStyle = QStringLiteral(
            "QFrame#composerAttachmentPill {"
            "  background-color: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 11px;"
            "}"
        ).arg(qssColor(u.background), qssColor(solidBorder));

        const QString removeStyle = QStringLiteral(
            "QToolButton#composerAttachmentRemove {"
            "  background-color: transparent;"
            "  color: %1;"
            "  border: none;"
            "  border-radius: 7px;"
            "  padding: 0px;"
            "  font-size: 11px;"
            "  font-weight: 700;"
            "}"
            "QToolButton#composerAttachmentRemove:hover {"
            "  background-color: %2;"
            "  color: %3;"
            "}"
        ).arg(qssColor(solidMuted), qssColor(solidElevated), qssColor(u.textPrimary));

        for (const QString& path : attachedFiles) {
            auto* pill = new QFrame(attachmentsRow);
            pill->setObjectName(QStringLiteral("composerAttachmentPill"));
            pill->setAttribute(Qt::WA_StyledBackground, true);
            pill->setToolTip(path);
            pill->setStyleSheet(pillStyle);
            pill->setFixedHeight(22);

            auto* pillLayout = new QHBoxLayout(pill);
            pillLayout->setContentsMargins(8, 0, 4, 0);
            pillLayout->setSpacing(4);

            auto* iconLabel = new QLabel(pill);
            iconLabel->setPixmap(icons::vectorIcon(QStringLiteral("file"), solidSecondary, 11).pixmap(11, 11));
            iconLabel->setFixedSize(11, 11);
            pillLayout->addWidget(iconLabel);

            const QFileInfo fi(path);
            const QString baseName = fi.fileName().isEmpty() ? path : fi.fileName();
            auto* nameLabel = new QLabel(baseName, pill);
            nameLabel->setFont(pillFont);
            nameLabel->setToolTip(path);
            nameLabel->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                         .arg(qssColor(u.textPrimary)));
            pillLayout->addWidget(nameLabel);

            auto* removeBtn = new QToolButton(pill);
            removeBtn->setObjectName(QStringLiteral("composerAttachmentRemove"));
            removeBtn->setText(QStringLiteral("\u00D7"));
            removeBtn->setFixedSize(15, 15);
            removeBtn->setCursor(Qt::PointingHandCursor);
            removeBtn->setFocusPolicy(Qt::TabFocus);
            removeBtn->setToolTip(QStringLiteral("Remove %1").arg(baseName));
            removeBtn->setStyleSheet(removeStyle);
            QObject::connect(removeBtn, &QToolButton::clicked, owner, [this, path]() {
                owner->removeAttachedFile(path);
            });
            pillLayout->addWidget(removeBtn);

            attachmentsLayout->addWidget(pill);
        }

        attachmentsLayout->addStretch(1);
        attachmentsScroll->setVisible(true);
        syncCompactAutoGrow();
    }

    void triggerSend() {
        if (!owner || !canSend()) {
            return;
        }
        const QString text = editor ? editor->toPlainText().trimmed() : QString();
        const QStringList files = attachedFiles;

        if (editor) {
            editor->clear();
        }
        if (!attachedFiles.isEmpty()) {
            attachedFiles.clear();
            rebuildAttachmentPills();
        }
        updateSendStopButton();
        syncCompactAutoGrow();

        emit owner->sendRequested(text, files);
    }
};

ComposerWidget::ComposerWidget(QWidget* parent)
    : QWidget(parent)
    , m_impl(std::make_unique<Impl>()) {
    m_impl->owner = this;

    setObjectName(QStringLiteral("composerWidget"));
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(true);
    setAcceptDrops(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(kOuterMargin, kOuterMargin, kOuterMargin, kOuterMargin);
    rootLayout->setSpacing(kRootSpacing);

    // -------------------------------------------------------------------------
    // Optional removable attached-file pill chips row (hidden when empty)
    // -------------------------------------------------------------------------
    m_impl->attachmentsScroll = new QScrollArea(this);
    m_impl->attachmentsScroll->setObjectName(QStringLiteral("composerAttachmentsScroll"));
    m_impl->attachmentsScroll->setFrameShape(QFrame::NoFrame);
    m_impl->attachmentsScroll->setWidgetResizable(true);
    m_impl->attachmentsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_impl->attachmentsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_impl->attachmentsScroll->setFixedHeight(kAttachmentsStripHeight);
    m_impl->attachmentsScroll->setVisible(false);

    m_impl->attachmentsRow = new QWidget(m_impl->attachmentsScroll);
    m_impl->attachmentsRow->setObjectName(QStringLiteral("composerAttachmentsRow"));
    m_impl->attachmentsLayout = new QHBoxLayout(m_impl->attachmentsRow);
    m_impl->attachmentsLayout->setContentsMargins(0, 1, 0, 1);
    m_impl->attachmentsLayout->setSpacing(6);
    m_impl->attachmentsLayout->addStretch(1);

    m_impl->attachmentsScroll->setWidget(m_impl->attachmentsRow);
    rootLayout->addWidget(m_impl->attachmentsScroll);

    // -------------------------------------------------------------------------
    // Single rounded monochrome composer container
    // -------------------------------------------------------------------------
    m_impl->composerCard = new QFrame(this);
    m_impl->composerCard->setObjectName(QStringLiteral("composerCard"));
    m_impl->composerCard->setAttribute(Qt::WA_StyledBackground, true);
    m_impl->composerCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_impl->composerCard->installEventFilter(this);

    auto* cardLayout = new QVBoxLayout(m_impl->composerCard);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    // Top: Multi-line QPlainTextEdit
    auto* editorLayout = new QVBoxLayout();
    editorLayout->setContentsMargins(
        kEditorLeftMargin, kEditorTopMargin, kEditorRightMargin, kEditorBottomMargin);
    editorLayout->setSpacing(0);

    m_impl->editor = new QPlainTextEdit(m_impl->composerCard);
    m_impl->editor->setObjectName(QStringLiteral("composerInput"));
    m_impl->editor->setFrameShape(QFrame::NoFrame);
    m_impl->editor->setPlaceholderText(QStringLiteral("Message BeatrixLeslie\u2026"));
    m_impl->editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_impl->editor->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    m_impl->editor->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_impl->editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_impl->editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_impl->editor->setMinimumHeight(kCompactMinEditorHeight);
    m_impl->editor->document()->setDocumentMargin(4);
    m_impl->editor->installEventFilter(this);
    m_impl->editor->viewport()->installEventFilter(this);

    QFont inputFont = font();
    inputFont.setPointSizeF(13.0);
    m_impl->editor->setFont(inputFont);

    connect(m_impl->editor, &QPlainTextEdit::textChanged, this, [this]() {
        m_impl->updateSendStopButton();
        m_impl->syncCompactAutoGrow();
    });

    if (auto* docLayout = m_impl->editor->document()->documentLayout()) {
        connect(docLayout, &QAbstractTextDocumentLayout::documentSizeChanged, this, [this](const QSizeF&) {
            m_impl->syncCompactAutoGrow();
        });
    }

    editorLayout->addWidget(m_impl->editor, 1);
    cardLayout->addLayout(editorLayout, 1);

    // Bottom button strip INSIDE the rounded frame (padding: 0 6px 6px 6px, spacing: 4px)
    auto* bottomBarLayout = new QHBoxLayout();
    bottomBarLayout->setContentsMargins(6, 0, 6, 6);
    bottomBarLayout->setSpacing(4);

    m_impl->expandButton = new QPushButton(m_impl->composerCard);
    m_impl->expandButton->setObjectName(QStringLiteral("composerExpandButton"));
    m_impl->expandButton->setFixedSize(kButtonWidth, kButtonHeight);
    m_impl->expandButton->setCursor(Qt::PointingHandCursor);
    m_impl->expandButton->setFocusPolicy(Qt::TabFocus);
    connect(m_impl->expandButton, &QPushButton::clicked, this, [this]() {
        setExpanded(!m_impl->expanded);
        focusInput();
    });
    bottomBarLayout->addWidget(m_impl->expandButton);

    m_impl->popOutTerminalButton = new QPushButton(m_impl->composerCard);
    m_impl->popOutTerminalButton->setObjectName(QStringLiteral("composerPopOutTerminalButton"));
    m_impl->popOutTerminalButton->setFixedSize(kButtonWidth, kButtonHeight);
    m_impl->popOutTerminalButton->setCursor(Qt::PointingHandCursor);
    m_impl->popOutTerminalButton->setFocusPolicy(Qt::TabFocus);
    connect(m_impl->popOutTerminalButton, &QPushButton::clicked, this, [this]() {
        const bool hasDedicatedReceiver = isSignalConnected(
            QMetaMethod::fromSignal(&ComposerWidget::popOutTerminalRequested));
        emit popOutTerminalRequested();
        if (!hasDedicatedReceiver) {
            emit terminalToggled();
        }
    });
    bottomBarLayout->addWidget(m_impl->popOutTerminalButton);

    bottomBarLayout->addStretch(1);

    m_impl->sendStopButton = new QPushButton(m_impl->composerCard);
    m_impl->sendStopButton->setObjectName(QStringLiteral("composerSendStopButton"));
    m_impl->sendStopButton->setFixedSize(kButtonWidth, kButtonHeight);
    m_impl->sendStopButton->setCursor(Qt::PointingHandCursor);
    m_impl->sendStopButton->setFocusPolicy(Qt::TabFocus);
    connect(m_impl->sendStopButton, &QPushButton::clicked, this, [this]() {
        if (m_impl->busy) {
            emit stopRequested();
        } else {
            m_impl->triggerSend();
        }
    });
    bottomBarLayout->addWidget(m_impl->sendStopButton);

    cardLayout->addLayout(bottomBarLayout);
    rootLayout->addWidget(m_impl->composerCard, 1);

    applyTheme();

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &ComposerWidget::applyTheme);
}

ComposerWidget::~ComposerWidget() = default;

void ComposerWidget::setBusy(bool busy) {
    if (m_impl->busy == busy) {
        return;
    }
    m_impl->busy = busy;
    m_impl->updateSendStopButton();
}

void ComposerWidget::setEndpointStatus(bool configured, const QString& modelId, bool online) {
    m_impl->endpointConfigured = configured;
    m_impl->modelId = modelId.trimmed();
    m_impl->endpointOnline = online;
}

void ComposerWidget::setThreadProfile(const QString& profile) {
    const QString normalized =
        (profile.trimmed().compare(QLatin1StringView("ask"), Qt::CaseInsensitive) == 0)
            ? QStringLiteral("ask")
            : QStringLiteral("write");
    if (m_impl->threadProfile == normalized) {
        return;
    }
    m_impl->threadProfile = normalized;
    m_impl->updateProfileButtonVisual();
}

QString ComposerWidget::threadProfile() const {
    return m_impl->threadProfile;
}

void ComposerWidget::setStageWriteMode(bool enabled) {
    m_impl->stageWriteMode = enabled;
}

void ComposerWidget::setTerminalVisible(bool visible) {
    m_impl->terminalVisible = visible;
}

void ComposerWidget::setAvailableTags(const QVector<RubricTag>& tags, const QStringList& activeTagIds) {
    m_impl->availableTags = tags;
    m_impl->activeTagIds.clear();
    m_impl->activeTagIds.reserve(activeTagIds.size());
    for (const QString& rawId : activeTagIds) {
        const QString trimmed = rawId.trimmed();
        if (!trimmed.isEmpty() && !m_impl->activeTagIds.contains(trimmed)) {
            m_impl->activeTagIds.append(trimmed);
        }
    }
}

bool ComposerWidget::isExpanded() const {
    return m_impl->expanded;
}

void ComposerWidget::setExpanded(bool expanded) {
    if (m_impl->expanded == expanded) {
        return;
    }
    m_impl->expanded = expanded;
    m_impl->autoGrownCompactHeight = 0;
    m_impl->preAutoGrowHeight = 0;
    m_impl->updateExpandButtonVisual();
    updateGeometry();
    m_impl->adjustParentSplitterHeight(m_impl->desiredTotalHeight());
    emit expandedToggled(expanded);
}

void ComposerWidget::focusInput() {
    if (m_impl->editor) {
        m_impl->editor->setFocus(Qt::OtherFocusReason);
    }
}

void ComposerWidget::applyTheme() {
    const UiPalette u = ThemeManager::instance().ui();
    const QColor solidBorder = compositeOver(u.subtleBorder, u.background);
    const QColor solidSelected = compositeOver(u.selectedFill, u.background);
    const QColor solidMuted = compositeOver(u.textMuted, u.background);

    QPalette rootPal = palette();
    rootPal.setColor(QPalette::Window, u.background);
    rootPal.setColor(QPalette::WindowText, u.textPrimary);
    setPalette(rootPal);

    setStyleSheet(QStringLiteral(
        "QWidget#composerWidget {"
        "  background-color: %1;"
        "}"
    ).arg(qssColor(u.background)));

    m_impl->updateComposerCardStyle();

    if (m_impl->attachmentsScroll) {
        m_impl->attachmentsScroll->setStyleSheet(QStringLiteral(
            "QScrollArea#composerAttachmentsScroll {"
            "  background-color: transparent;"
            "  border: none;"
            "}"
        ));
    }
    if (m_impl->attachmentsRow) {
        m_impl->attachmentsRow->setStyleSheet(QStringLiteral(
            "QWidget#composerAttachmentsRow { background-color: transparent; }"
        ));
    }
    if (!m_impl->attachedFiles.isEmpty()) {
        m_impl->rebuildAttachmentPills();
    }

    if (m_impl->editor) {
        QFont inputFont = font();
        inputFont.setPointSizeF(13.0);
        m_impl->editor->setFont(inputFont);

        QPalette editorPal = m_impl->editor->palette();
        editorPal.setColor(QPalette::Base, u.background);
        editorPal.setColor(QPalette::Text, u.textPrimary);
        editorPal.setColor(QPalette::PlaceholderText, solidMuted);
        editorPal.setColor(QPalette::Highlight, solidSelected);
        editorPal.setColor(QPalette::HighlightedText, u.textPrimary);
        m_impl->editor->setPalette(editorPal);

        m_impl->editor->setStyleSheet(QStringLiteral(
            "QPlainTextEdit#composerInput,"
            "QPlainTextEdit#composerInput:focus {"
            "  background-color: transparent;"
            "  color: %1;"
            "  border: none;"
            "  border-radius: 0px;"
            "  padding: 0px;"
            "  selection-background-color: %2;"
            "  selection-color: %1;"
            "}"
            "QPlainTextEdit#composerInput QScrollBar:vertical {"
            "  background: transparent;"
            "  width: 6px;"
            "  margin: 2px 0px 2px 0px;"
            "}"
            "QPlainTextEdit#composerInput QScrollBar::handle:vertical {"
            "  background: %3;"
            "  border-radius: 3px;"
            "  min-height: 20px;"
            "}"
            "QPlainTextEdit#composerInput QScrollBar::handle:vertical:hover {"
            "  background: %4;"
            "}"
            "QPlainTextEdit#composerInput QScrollBar::add-line:vertical,"
            "QPlainTextEdit#composerInput QScrollBar::sub-line:vertical,"
            "QPlainTextEdit#composerInput QScrollBar::add-page:vertical,"
            "QPlainTextEdit#composerInput QScrollBar::sub-page:vertical {"
            "  background: transparent;"
            "  height: 0px;"
            "}"
        ).arg(qssColor(u.textPrimary), qssColor(solidSelected),
              qssColor(solidBorder), qssColor(solidMuted)));
    }

    m_impl->updateExpandButtonVisual();
    m_impl->updatePopOutTerminalButtonVisual();
    m_impl->updateProfileButtonVisual();
    m_impl->updateSendStopButton(true);
}

QString ComposerWidget::inputText() const {
    return m_impl->editor ? m_impl->editor->toPlainText() : QString();
}

void ComposerWidget::setInputText(const QString& text) {
    if (!m_impl->editor) {
        return;
    }
    m_impl->editor->setPlainText(text);
    QTextCursor cursor = m_impl->editor->textCursor();
    cursor.movePosition(QTextCursor::End);
    m_impl->editor->setTextCursor(cursor);
    m_impl->updateSendStopButton();
    m_impl->syncCompactAutoGrow();
}

void ComposerWidget::clearInput() {
    if (!m_impl->editor) {
        return;
    }
    m_impl->editor->clear();
    m_impl->updateSendStopButton();
    m_impl->syncCompactAutoGrow();
}

QStringList ComposerWidget::attachedFiles() const {
    return m_impl->attachedFiles;
}

void ComposerWidget::addAttachedFile(const QString& filePath) {
    const QString trimmed = filePath.trimmed();
    if (trimmed.isEmpty() || m_impl->attachedFiles.contains(trimmed)) {
        return;
    }
    m_impl->attachedFiles.append(trimmed);
    m_impl->rebuildAttachmentPills();
    m_impl->updateSendStopButton();
}

void ComposerWidget::removeAttachedFile(const QString& filePath) {
    const int removed = static_cast<int>(m_impl->attachedFiles.removeAll(filePath))
        + static_cast<int>(m_impl->attachedFiles.removeAll(filePath.trimmed()));
    if (removed == 0) {
        return;
    }
    m_impl->rebuildAttachmentPills();
    m_impl->updateSendStopButton();
}

void ComposerWidget::clearAttachedFiles() {
    if (m_impl->attachedFiles.isEmpty()) {
        return;
    }
    m_impl->attachedFiles.clear();
    m_impl->rebuildAttachmentPills();
    m_impl->updateSendStopButton();
}

bool ComposerWidget::isBusy() const {
    return m_impl->busy;
}

bool ComposerWidget::isStageWriteMode() const {
    return m_impl->stageWriteMode;
}

bool ComposerWidget::isTerminalVisible() const {
    return m_impl->terminalVisible;
}

QStringList ComposerWidget::activeTagIds() const {
    return m_impl->activeTagIds;
}

bool ComposerWidget::canSend() const {
    return m_impl->canSend();
}

QSize ComposerWidget::sizeHint() const {
    return QSize(420, m_impl->desiredTotalHeight());
}

QSize ComposerWidget::minimumSizeHint() const {
    return QSize(260, m_impl->chromeHeight() + kCompactMinEditorHeight);
}

bool ComposerWidget::eventFilter(QObject* watched, QEvent* event) {
    if (!event) {
        return QWidget::eventFilter(watched, event);
    }

    if (watched == m_impl->editor) {
        if (event->type() == QEvent::KeyPress) {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            const int key = keyEvent->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                const Qt::KeyboardModifiers mods = keyEvent->modifiers() & ~Qt::KeypadModifier;
                if (mods == Qt::ControlModifier || mods == Qt::MetaModifier) {
                    m_impl->triggerSend();
                    return true;
                }
                if (mods == Qt::ShiftModifier) {
                    m_impl->editor->insertPlainText(QStringLiteral("\n"));
                    m_impl->editor->ensureCursorVisible();
                    return true;
                }
                if (mods == Qt::NoModifier) {
                    if (!m_impl->expanded) {
                        m_impl->triggerSend();
                        return true;
                    }
                    // Expanded mode: plain Enter inserts a newline.
                    return false;
                }
            }
        } else if (event->type() == QEvent::FocusIn) {
            if (!m_impl->editorFocused) {
                m_impl->editorFocused = true;
                m_impl->updateComposerCardStyle();
            }
        } else if (event->type() == QEvent::FocusOut) {
            if (m_impl->editorFocused) {
                m_impl->editorFocused = false;
                m_impl->updateComposerCardStyle();
            }
        } else if (event->type() == QEvent::DragEnter) {
            auto* dragEvent = static_cast<QDragEnterEvent*>(event);
            if (hasAttachableLocalFiles(dragEvent->mimeData())) {
                dragEvent->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::DragMove) {
            auto* dragEvent = static_cast<QDragMoveEvent*>(event);
            if (hasAttachableLocalFiles(dragEvent->mimeData())) {
                dragEvent->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto* dropEvent = static_cast<QDropEvent*>(event);
            const QStringList paths = extractAttachableLocalFiles(dropEvent->mimeData());
            if (!paths.isEmpty()) {
                for (const QString& path : paths) {
                    addAttachedFile(path);
                }
                dropEvent->acceptProposedAction();
                return true;
            }
        }
    } else if (m_impl->editor && watched == m_impl->editor->viewport()) {
        if (event->type() == QEvent::DragEnter) {
            auto* dragEvent = static_cast<QDragEnterEvent*>(event);
            if (hasAttachableLocalFiles(dragEvent->mimeData())) {
                dragEvent->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::DragMove) {
            auto* dragEvent = static_cast<QDragMoveEvent*>(event);
            if (hasAttachableLocalFiles(dragEvent->mimeData())) {
                dragEvent->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto* dropEvent = static_cast<QDropEvent*>(event);
            const QStringList paths = extractAttachableLocalFiles(dropEvent->mimeData());
            if (!paths.isEmpty()) {
                for (const QString& path : paths) {
                    addAttachedFile(path);
                }
                dropEvent->acceptProposedAction();
                return true;
            }
        }
    } else if (watched == m_impl->composerCard) {
        if (event->type() == QEvent::MouseButtonPress) {
            focusInput();
        }
    }

    return QWidget::eventFilter(watched, event);
}

void ComposerWidget::dragEnterEvent(QDragEnterEvent* event) {
    if (event && hasAttachableLocalFiles(event->mimeData())) {
        event->acceptProposedAction();
        return;
    }
    QWidget::dragEnterEvent(event);
}

void ComposerWidget::dropEvent(QDropEvent* event) {
    if (event) {
        const QStringList paths = extractAttachableLocalFiles(event->mimeData());
        if (!paths.isEmpty()) {
            for (const QString& path : paths) {
                addAttachedFile(path);
            }
            event->acceptProposedAction();
            return;
        }
    }
    QWidget::dropEvent(event);
}

} // namespace spell
