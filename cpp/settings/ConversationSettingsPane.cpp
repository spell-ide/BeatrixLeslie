#include "settings/ConversationSettingsPane.h"

#include "core/SpellCoreClient.h"
#include "theme/SpellIcons.h"
#include "theme/SpellTheme.h"
#include "widgets/SpellBadges.h"

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <utility>

namespace spell {

namespace {

enum class FormKind {
    Tag,
    Steering
};

struct SteeringDirectiveItem {
    QString id;
    QString name;
    QString trigger;
    QString instruction;
    bool isBuiltIn = false;

    bool operator==(const SteeringDirectiveItem&) const = default;
};

QVector<SteeringDirectiveItem> defaultBuiltInSteeringDirectives() {
    return {
        SteeringDirectiveItem{
            QStringLiteral("deep-think"),
            QStringLiteral("Deep Think"),
            QStringLiteral("#steer:think"),
            QStringLiteral("Enable extended thinking mode with calibrated reasoning sampling."),
            true,
        },
        SteeringDirectiveItem{
            QStringLiteral("fast-patch"),
            QStringLiteral("Fast Patch"),
            QStringLiteral("#steer:fast"),
            QStringLiteral("Output only unified diffs or exact replacement blocks. Zero prose."),
            true,
        },
        SteeringDirectiveItem{
            QStringLiteral("concise"),
            QStringLiteral("Concise"),
            QStringLiteral("#steer:concise"),
            QStringLiteral("Keep responses concise, structured, and high-signal."),
            true,
        },
        SteeringDirectiveItem{
            QStringLiteral("deterministic"),
            QStringLiteral("Deterministic"),
            QStringLiteral("#steer:deterministic"),
            QStringLiteral("Greedy deterministic decoding (temperature = 0.0, top_p = 1.0)."),
            true,
        },
        SteeringDirectiveItem{
            QStringLiteral("spell"),
            QStringLiteral("Spell CLI Handoff"),
            QStringLiteral("#spell"),
            QStringLiteral("Terminal #spell handoff: inspect the attached working directory and terminal output."),
            true,
        },
    };
}

QVector<SteeringDirectiveItem> parseSteeringDirectives(const QJsonObject& raw) {
    const QJsonArray arr = detail::getArray(raw, {"available_steering", "availableSteering", "steering"});
    if (arr.isEmpty()) {
        return defaultBuiltInSteeringDirectives();
    }

    QVector<SteeringDirectiveItem> out;
    out.reserve(arr.size());
    for (const QJsonValue& val : arr) {
        if (!val.isObject()) {
            continue;
        }
        const QJsonObject obj = val.toObject();
        SteeringDirectiveItem item;
        item.id = detail::getString(obj, {"id", "slug", "name"}).trimmed();
        item.name = detail::getString(obj, {"name", "title", "id"}, item.id).trimmed();
        item.trigger = detail::getString(obj, {"trigger"}).trimmed();
        if (item.trigger.isEmpty() && !item.id.isEmpty()) {
            item.trigger = QStringLiteral("#steer:") + item.id;
        }
        item.instruction = detail::getString(
            obj,
            {"instruction", "system_append", "systemAppend", "directive", "rubric_prompt", "rubricPrompt", "description"});
        item.isBuiltIn = detail::getBool(
            obj, {"is_builtin", "isBuiltin", "is_built_in", "isBuiltIn", "builtin"}, false);
        if (!item.id.isEmpty()) {
            out.append(std::move(item));
        }
    }
    return out.isEmpty() ? defaultBuiltInSteeringDirectives() : out;
}

QStringList parseActiveSteeringIds(const QJsonObject& raw) {
    return detail::getStringList(raw, {"active_steering", "activeSteering"});
}

QString slugifyIdentifier(const QString& input) {
    QString out;
    const QString trimmed = input.trimmed().toLower();
    out.reserve(trimmed.size());
    bool lastWasDash = false;
    for (const QChar ch : trimmed) {
        if (ch.isLetterOrNumber()) {
            out.append(ch);
            lastWasDash = false;
        } else if ((ch == QLatin1Char('-') || ch == QLatin1Char('_') || ch.isSpace()) && !out.isEmpty() && !lastWasDash) {
            out.append(QLatin1Char('-'));
            lastWasDash = true;
        }
    }
    while (out.endsWith(QLatin1Char('-'))) {
        out.chop(1);
    }
    return out;
}

bool listContainsTag(const QStringList& activeIds, const RubricTag& tag) {
    for (const QString& entry : activeIds) {
        if (!tag.id.isEmpty() && entry.compare(tag.id, Qt::CaseInsensitive) == 0) {
            return true;
        }
        if (!tag.name.isEmpty() && entry.compare(tag.name, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

bool listContainsSteering(const QStringList& activeIds, const SteeringDirectiveItem& item) {
    for (const QString& entry : activeIds) {
        if (!item.id.isEmpty() && entry.compare(item.id, Qt::CaseInsensitive) == 0) {
            return true;
        }
        if (!item.trigger.isEmpty() && entry.compare(item.trigger, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QString resolveThreadTitle(const WorkspaceSnapshot& snap) {
    if (snap.selectedThreadId.isEmpty()) {
        return {};
    }
    for (const ThreadSummary& thread : snap.threads) {
        if (thread.id == snap.selectedThreadId) {
            return thread.title.trimmed().isEmpty() ? thread.id : thread.title.trimmed();
        }
    }
    return snap.selectedThreadId;
}

QString compactPreviewText(const QString& raw, int maxChars = 190) {
    QString collapsed = raw.simplified();
    if (collapsed.size() <= maxChars) {
        return collapsed;
    }
    return collapsed.left(maxChars - 1).trimmed() + QStringLiteral("…");
}

void clearLayoutDeferred(QLayout* layout) {
    if (!layout) {
        return;
    }
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        }
        if (QLayout* childLayout = item->layout()) {
            clearLayoutDeferred(childLayout);
        }
        delete item;
    }
}

void configureNoAutoDefault(QPushButton* btn) {
    if (!btn) {
        return;
    }
    btn->setAutoDefault(false);
    btn->setDefault(false);
}

void ensureVisibleInAncestorScrollArea(QWidget* widget, int xMargin = 0, int yMargin = 24) {
    if (!widget) {
        return;
    }
    for (QWidget* ancestor = widget->parentWidget(); ancestor != nullptr; ancestor = ancestor->parentWidget()) {
        if (auto* scroll = qobject_cast<QScrollArea*>(ancestor)) {
            if (scroll->widget() && scroll->widget()->isAncestorOf(widget)) {
                scroll->ensureWidgetVisible(widget, xMargin, yMargin);
                return;
            }
        }
    }
}

/**
 * Lightweight Qt 6 wrapping flow layout (no Q_OBJECT needed) for chip bars.
 */
class FlowLayout final : public QLayout {
public:
    explicit FlowLayout(QWidget* parent = nullptr, int margin = 0, int hSpacing = 6, int vSpacing = 6)
        : QLayout(parent)
        , m_hSpace(hSpacing)
        , m_vSpace(vSpacing) {
        setContentsMargins(margin, margin, margin, margin);
    }

    ~FlowLayout() override {
        QLayoutItem* item = nullptr;
        while ((item = takeAt(0)) != nullptr) {
            delete item;
        }
    }

    void addItem(QLayoutItem* item) override {
        m_items.append(item);
    }

    int horizontalSpacing() const {
        if (m_hSpace >= 0) {
            return m_hSpace;
        }
        return smartSpacing(QStyle::PM_LayoutHorizontalSpacing);
    }

    int verticalSpacing() const {
        if (m_vSpace >= 0) {
            return m_vSpace;
        }
        return smartSpacing(QStyle::PM_LayoutVerticalSpacing);
    }

    Qt::Orientations expandingDirections() const override {
        return {};
    }

    bool hasHeightForWidth() const override {
        return true;
    }

    int heightForWidth(int width) const override {
        return doLayout(QRect(0, 0, width, 0), true);
    }

    int count() const override {
        return static_cast<int>(m_items.size());
    }

    QLayoutItem* itemAt(int index) const override {
        return (index >= 0 && index < m_items.size()) ? m_items.at(index) : nullptr;
    }

    QSize minimumSize() const override {
        QSize size;
        for (const QLayoutItem* item : m_items) {
            size = size.expandedTo(item->minimumSize());
        }
        const QMargins margins = contentsMargins();
        size += QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
        return size;
    }

    void setGeometry(const QRect& rect) override {
        QLayout::setGeometry(rect);
        doLayout(rect, false);
    }

    QSize sizeHint() const override {
        return minimumSize();
    }

    QLayoutItem* takeAt(int index) override {
        if (index >= 0 && index < m_items.size()) {
            return m_items.takeAt(index);
        }
        return nullptr;
    }

private:
    int doLayout(const QRect& rect, bool testOnly) const {
        const QMargins margins = contentsMargins();
        const QRect effectiveRect = rect.adjusted(
            margins.left(), margins.top(), -margins.right(), -margins.bottom());
        int x = effectiveRect.x();
        int y = effectiveRect.y();
        int lineHeight = 0;

        for (QLayoutItem* item : m_items) {
            const QWidget* wid = item->widget();
            int spaceX = horizontalSpacing();
            if (spaceX == -1 && wid) {
                spaceX = wid->style()->layoutSpacing(
                    QSizePolicy::PushButton, QSizePolicy::PushButton, Qt::Horizontal);
            }
            int spaceY = verticalSpacing();
            if (spaceY == -1 && wid) {
                spaceY = wid->style()->layoutSpacing(
                    QSizePolicy::PushButton, QSizePolicy::PushButton, Qt::Vertical);
            }
            if (spaceX < 0) {
                spaceX = 6;
            }
            if (spaceY < 0) {
                spaceY = 6;
            }

            const QSize hint = item->sizeHint();
            int nextX = x + hint.width() + spaceX;
            if (nextX - spaceX > effectiveRect.right() && lineHeight > 0) {
                x = effectiveRect.x();
                y += lineHeight + spaceY;
                nextX = x + hint.width() + spaceX;
                lineHeight = 0;
            }

            if (!testOnly) {
                item->setGeometry(QRect(QPoint(x, y), hint));
            }

            x = nextX;
            lineHeight = std::max(lineHeight, hint.height());
        }
        return y + lineHeight - rect.y() + margins.bottom();
    }

    int smartSpacing(QStyle::PixelMetric pm) const {
        QObject* parentObj = parent();
        if (!parentObj) {
            return -1;
        }
        if (parentObj->isWidgetType()) {
            auto* pw = static_cast<QWidget*>(parentObj);
            return pw->style()->pixelMetric(pm, nullptr, pw);
        }
        return static_cast<QLayout*>(parentObj)->spacing();
    }

    QVector<QLayoutItem*> m_items;
    int m_hSpace = 6;
    int m_vSpace = 6;
};

} // namespace

struct ConversationSettingsPane::Impl {
    ConversationSettingsPane* owner = nullptr;
    SpellCoreClient* client = nullptr;

    QLabel* introCaption = nullptr;

    // Section 1: Active conversation thread tags
    QFrame* activeTagsCard = nullptr;
    SpellSectionHeader* activeTagsHeader = nullptr;
    SpellBadge* activeTagsCountBadge = nullptr;
    QPushButton* clearActiveTagsBtn = nullptr;
    QLabel* activeThreadCaption = nullptr;
    QWidget* activeTagsChipHost = nullptr;
    FlowLayout* activeTagsFlow = nullptr;
    QLabel* activeTagsEmptyLabel = nullptr;

    // Section 2: Steering directives
    QFrame* steeringCard = nullptr;
    SpellSectionHeader* steeringHeader = nullptr;
    SpellBadge* steeringCountBadge = nullptr;
    QLabel* steeringHintLabel = nullptr;
    QWidget* steeringChipHost = nullptr;
    FlowLayout* steeringFlow = nullptr;

    // Section 3: Rubric Tags Manager list
    QFrame* registryCard = nullptr;
    SpellSectionHeader* registryHeader = nullptr;
    SpellBadge* registryCountBadge = nullptr;
    QLineEdit* tagFilterEdit = nullptr;
    QPushButton* newTagBtn = nullptr;
    QWidget* tagListHost = nullptr;
    QVBoxLayout* tagListLayout = nullptr;
    QLabel* tagListEmptyLabel = nullptr;

    // Section 4: Add / Edit Tag & Steering Form
    QFrame* formCard = nullptr;
    SpellSectionHeader* formHeader = nullptr;
    SpellBadge* editingBadge = nullptr;
    QPushButton* kindTagBtn = nullptr;
    QPushButton* kindSteeringBtn = nullptr;
    QLabel* nameLabel = nullptr;
    QLineEdit* nameEdit = nullptr;
    QLabel* descriptionLabel = nullptr;
    QLineEdit* descriptionEdit = nullptr;
    QLabel* directiveLabel = nullptr;
    QPlainTextEdit* directiveEdit = nullptr;
    QWidget* enabledRowWidget = nullptr;
    QCheckBox* formEnabledCheck = nullptr;
    QLabel* triggerPreviewLabel = nullptr;
    SpellHairline* formDivider = nullptr;
    QLabel* statusLabel = nullptr;
    QPushButton* resetBtn = nullptr;
    QPushButton* submitBtn = nullptr;

    // Cached snapshot data (prevents unnecessary widget churn on 30ms polling)
    bool hasSnapshot = false;
    QString selectedThreadId;
    QString selectedThreadTitle;
    QVector<RubricTag> tags;
    QStringList activeThreadTagIds;
    QVector<SteeringDirectiveItem> steeringDirectives = defaultBuiltInSteeringDirectives();
    QStringList activeSteeringIds;

    // Form editing state
    FormKind formKind = FormKind::Tag;
    QString editingItemId;
    bool statusIsError = false;

    void setupUi();
    void rebuildActiveTagsSection();
    void rebuildSteeringSection();
    void rebuildTagListSection();
    void updateFormModeUi();
    void updateTriggerPreview();
    void updateSubmitEnabled();
    void setStatus(const QString& text, bool isError = false);

    void beginCreateMode(FormKind kind, bool focusNameField);
    void beginEditTag(const RubricTag& tag);
    void beginEditSteering(const SteeringDirectiveItem& item);
    void submitForm();
    void toggleTagOnSelectedThread(const RubricTag& tag);
    void clearSelectedThreadTags();
    void toggleTagEnabled(const RubricTag& tag, bool enabled);
    void deleteRubricTag(const RubricTag& tag);
    void toggleSteeringDirective(const SteeringDirectiveItem& item);

    void applyTheme();
};

void ConversationSettingsPane::Impl::setupUi() {
    owner->setObjectName(QStringLiteral("conversationSettingsPane"));
    owner->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* mainLayout = new QVBoxLayout(owner);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(14);

    introCaption = new QLabel(
        QStringLiteral("Attach rubric tags to the active conversation, toggle per-turn steering directives, "
                       "and manage custom rubrics persisted in BeatrixLeslie IDE."),
        owner);
    introCaption->setWordWrap(true);
    mainLayout->addWidget(introCaption);

    // -------------------------------------------------------------------------
    // Card 1: Active Conversation Rubric Tags
    // -------------------------------------------------------------------------
    activeTagsCard = new QFrame(owner);
    activeTagsCard->setObjectName(QStringLiteral("spellSettingsCard"));
    auto* activeCardLayout = new QVBoxLayout(activeTagsCard);
    activeCardLayout->setContentsMargins(12, 12, 12, 12);
    activeCardLayout->setSpacing(8);

    auto* activeTopRow = new QHBoxLayout();
    activeTopRow->setContentsMargins(0, 0, 0, 0);
    activeTopRow->setSpacing(8);

    activeTagsHeader = new SpellSectionHeader(
        QStringLiteral("Active Conversation Tags"), QStringLiteral("tag"), BadgeTone::Accent, activeTagsCard);
    activeTopRow->addWidget(activeTagsHeader);

    activeTagsCountBadge = new SpellBadge(QStringLiteral("0 ACTIVE"), BadgeTone::Neutral, false, activeTagsCard);
    activeTopRow->addWidget(activeTagsCountBadge);
    activeTopRow->addStretch(1);

    clearActiveTagsBtn = new QPushButton(QStringLiteral("Clear Active"), activeTagsCard);
    clearActiveTagsBtn->setProperty("flat", true);
    clearActiveTagsBtn->setCursor(Qt::PointingHandCursor);
    configureNoAutoDefault(clearActiveTagsBtn);
    clearActiveTagsBtn->setVisible(false);
    QObject::connect(clearActiveTagsBtn, &QPushButton::clicked, owner, [this]() {
        clearSelectedThreadTags();
    });
    activeTopRow->addWidget(clearActiveTagsBtn);
    activeCardLayout->addLayout(activeTopRow);

    activeThreadCaption = new QLabel(
        QStringLiteral("Select a conversation thread to apply rubric tags."), activeTagsCard);
    activeThreadCaption->setWordWrap(true);
    activeCardLayout->addWidget(activeThreadCaption);

    activeTagsChipHost = new QWidget(activeTagsCard);
    activeTagsChipHost->setObjectName(QStringLiteral("spellTransparentHost"));
    activeTagsChipHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    activeTagsFlow = new FlowLayout(activeTagsChipHost, 0, 6, 6);
    activeCardLayout->addWidget(activeTagsChipHost);

    activeTagsEmptyLabel = new QLabel(
        QStringLiteral("No enabled rubric tags available yet."), activeTagsCard);
    activeTagsEmptyLabel->setWordWrap(true);
    activeCardLayout->addWidget(activeTagsEmptyLabel);

    mainLayout->addWidget(activeTagsCard);

    // -------------------------------------------------------------------------
    // Card 2: Steering & Thinking Directives
    // -------------------------------------------------------------------------
    steeringCard = new QFrame(owner);
    steeringCard->setObjectName(QStringLiteral("spellSettingsCard"));
    auto* steeringCardLayout = new QVBoxLayout(steeringCard);
    steeringCardLayout->setContentsMargins(12, 12, 12, 12);
    steeringCardLayout->setSpacing(8);

    auto* steeringTopRow = new QHBoxLayout();
    steeringTopRow->setContentsMargins(0, 0, 0, 0);
    steeringTopRow->setSpacing(8);

    steeringHeader = new SpellSectionHeader(
        QStringLiteral("Steering & Thinking Directives"), QStringLiteral("sparkles"), BadgeTone::Accent, steeringCard);
    steeringTopRow->addWidget(steeringHeader);

    steeringCountBadge = new SpellBadge(QStringLiteral("0 ACTIVE"), BadgeTone::Neutral, false, steeringCard);
    steeringTopRow->addWidget(steeringCountBadge);
    steeringTopRow->addStretch(1);
    steeringCardLayout->addLayout(steeringTopRow);

    steeringHintLabel = new QLabel(
        QStringLiteral("Click a directive chip to toggle per-turn steering, or use its edit icon to customize instructions."),
        steeringCard);
    steeringHintLabel->setWordWrap(true);
    steeringCardLayout->addWidget(steeringHintLabel);

    steeringChipHost = new QWidget(steeringCard);
    steeringChipHost->setObjectName(QStringLiteral("spellTransparentHost"));
    steeringChipHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    steeringFlow = new FlowLayout(steeringChipHost, 0, 6, 6);
    steeringCardLayout->addWidget(steeringChipHost);

    mainLayout->addWidget(steeringCard);

    // -------------------------------------------------------------------------
    // Card 3: Rubric Tags Manager (list with enable checkbox, built-in badge, edit & delete)
    // -------------------------------------------------------------------------
    registryCard = new QFrame(owner);
    registryCard->setObjectName(QStringLiteral("spellSettingsCard"));
    auto* registryCardLayout = new QVBoxLayout(registryCard);
    registryCardLayout->setContentsMargins(12, 12, 12, 12);
    registryCardLayout->setSpacing(10);

    auto* registryTopRow = new QHBoxLayout();
    registryTopRow->setContentsMargins(0, 0, 0, 0);
    registryTopRow->setSpacing(8);

    registryHeader = new SpellSectionHeader(
        QStringLiteral("Rubric Tags"), QStringLiteral("tag"), BadgeTone::Accent, registryCard);
    registryTopRow->addWidget(registryHeader);

    registryCountBadge = new SpellBadge(QStringLiteral("0 TAGS"), BadgeTone::Neutral, false, registryCard);
    registryTopRow->addWidget(registryCountBadge);
    registryTopRow->addStretch(1);

    tagFilterEdit = new QLineEdit(registryCard);
    tagFilterEdit->setObjectName(QStringLiteral("tagFilterEdit"));
    tagFilterEdit->setPlaceholderText(QStringLiteral("Filter tags…"));
    tagFilterEdit->setClearButtonEnabled(true);
    tagFilterEdit->setMaximumWidth(190);
    QObject::connect(tagFilterEdit, &QLineEdit::textChanged, owner, [this]() {
        rebuildTagListSection();
    });
    registryTopRow->addWidget(tagFilterEdit);

    newTagBtn = new QPushButton(QStringLiteral("New Tag"), registryCard);
    newTagBtn->setObjectName(QStringLiteral("newTagButton"));
    newTagBtn->setCursor(Qt::PointingHandCursor);
    configureNoAutoDefault(newTagBtn);
    QObject::connect(newTagBtn, &QPushButton::clicked, owner, [this]() {
        beginCreateMode(FormKind::Tag, true);
    });
    registryTopRow->addWidget(newTagBtn);

    registryCardLayout->addLayout(registryTopRow);

    tagListHost = new QWidget(registryCard);
    tagListHost->setObjectName(QStringLiteral("spellTransparentHost"));
    tagListHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tagListLayout = new QVBoxLayout(tagListHost);
    tagListLayout->setContentsMargins(0, 0, 0, 0);
    tagListLayout->setSpacing(8);
    registryCardLayout->addWidget(tagListHost);

    tagListEmptyLabel = new QLabel(
        QStringLiteral("No rubric tags configured yet. Create one using the form below."), registryCard);
    tagListEmptyLabel->setWordWrap(true);
    registryCardLayout->addWidget(tagListEmptyLabel);

    mainLayout->addWidget(registryCard);

    // -------------------------------------------------------------------------
    // Card 4: Add / Edit Rubric Tag (or Steering Directive) Form
    // -------------------------------------------------------------------------
    formCard = new QFrame(owner);
    formCard->setObjectName(QStringLiteral("spellSettingsCard"));
    auto* formCardLayout = new QVBoxLayout(formCard);
    formCardLayout->setContentsMargins(12, 12, 12, 12);
    formCardLayout->setSpacing(9);

    auto* formTopRow = new QHBoxLayout();
    formTopRow->setContentsMargins(0, 0, 0, 0);
    formTopRow->setSpacing(8);

    formHeader = new SpellSectionHeader(
        QStringLiteral("Add Rubric Tag"), QStringLiteral("edit"), BadgeTone::Accent, formCard);
    formTopRow->addWidget(formHeader);

    editingBadge = new SpellBadge(QStringLiteral("EDITING"), BadgeTone::Info, true, formCard);
    editingBadge->setVisible(false);
    formTopRow->addWidget(editingBadge);
    formTopRow->addStretch(1);

    auto* kindToggleRow = new QHBoxLayout();
    kindToggleRow->setContentsMargins(0, 0, 0, 0);
    kindToggleRow->setSpacing(4);

    kindTagBtn = new QPushButton(QStringLiteral("Rubric Tag"), formCard);
    kindTagBtn->setCheckable(true);
    kindTagBtn->setChecked(true);
    kindTagBtn->setCursor(Qt::PointingHandCursor);
    configureNoAutoDefault(kindTagBtn);

    kindSteeringBtn = new QPushButton(QStringLiteral("Steering"), formCard);
    kindSteeringBtn->setCheckable(true);
    kindSteeringBtn->setChecked(false);
    kindSteeringBtn->setCursor(Qt::PointingHandCursor);
    configureNoAutoDefault(kindSteeringBtn);

    QObject::connect(kindTagBtn, &QPushButton::clicked, owner, [this]() {
        if (formKind != FormKind::Tag) {
            beginCreateMode(FormKind::Tag, false);
        } else {
            kindTagBtn->setChecked(true);
        }
    });
    QObject::connect(kindSteeringBtn, &QPushButton::clicked, owner, [this]() {
        if (formKind != FormKind::Steering) {
            beginCreateMode(FormKind::Steering, false);
        } else {
            kindSteeringBtn->setChecked(true);
        }
    });

    kindToggleRow->addWidget(kindTagBtn);
    kindToggleRow->addWidget(kindSteeringBtn);
    formTopRow->addLayout(kindToggleRow);
    formCardLayout->addLayout(formTopRow);

    // Name & Description side-by-side for compact layout
    auto* metaRow = new QHBoxLayout();
    metaRow->setContentsMargins(0, 0, 0, 0);
    metaRow->setSpacing(10);

    auto* nameCol = new QVBoxLayout();
    nameCol->setContentsMargins(0, 0, 0, 0);
    nameCol->setSpacing(4);
    nameLabel = new QLabel(QStringLiteral("Name"), formCard);
    nameEdit = new QLineEdit(formCard);
    nameEdit->setObjectName(QStringLiteral("tagNameEdit"));
    nameEdit->setPlaceholderText(QStringLiteral("e.g. Security Review"));
    nameCol->addWidget(nameLabel);
    nameCol->addWidget(nameEdit);
    metaRow->addLayout(nameCol, 1);

    auto* descCol = new QVBoxLayout();
    descCol->setContentsMargins(0, 0, 0, 0);
    descCol->setSpacing(4);
    descriptionLabel = new QLabel(QStringLiteral("Description"), formCard);
    descriptionEdit = new QLineEdit(formCard);
    descriptionEdit->setObjectName(QStringLiteral("tagDescriptionEdit"));
    descriptionEdit->setPlaceholderText(QStringLiteral("Short summary of when to apply this tag"));
    descCol->addWidget(descriptionLabel);
    descCol->addWidget(descriptionEdit);
    metaRow->addLayout(descCol, 2);

    formCardLayout->addLayout(metaRow);

    // Multiline Directive field
    auto* dirCol = new QVBoxLayout();
    dirCol->setContentsMargins(0, 0, 0, 0);
    dirCol->setSpacing(4);
    directiveLabel = new QLabel(QStringLiteral("Directive"), formCard);
    directiveEdit = new QPlainTextEdit(formCard);
    directiveEdit->setObjectName(QStringLiteral("tagDirectiveEdit"));
    directiveEdit->setPlaceholderText(
        QStringLiteral("Enter the rubric instructions or steering directive injected when active…"));
    directiveEdit->setMinimumHeight(92);
    directiveEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    directiveEdit->setTabChangesFocus(true);
    dirCol->addWidget(directiveLabel);
    dirCol->addWidget(directiveEdit);
    formCardLayout->addLayout(dirCol);

    // Enabled checkbox + live trigger preview
    enabledRowWidget = new QWidget(formCard);
    enabledRowWidget->setObjectName(QStringLiteral("spellTransparentHost"));
    auto* enabledRow = new QHBoxLayout(enabledRowWidget);
    enabledRow->setContentsMargins(0, 2, 0, 2);
    enabledRow->setSpacing(10);

    formEnabledCheck = new QCheckBox(QStringLiteral("Enabled"), enabledRowWidget);
    formEnabledCheck->setObjectName(QStringLiteral("tagEnabledCheck"));
    formEnabledCheck->setChecked(true);
    enabledRow->addWidget(formEnabledCheck);

    enabledRow->addStretch(1);

    triggerPreviewLabel = new QLabel(enabledRowWidget);
    triggerPreviewLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    enabledRow->addWidget(triggerPreviewLabel);

    formCardLayout->addWidget(enabledRowWidget);

    formDivider = new SpellHairline(Qt::Horizontal, formCard);
    formCardLayout->addWidget(formDivider);

    // Footer: status feedback + Cancel/Clear + Create/Save button
    auto* footerRow = new QHBoxLayout();
    footerRow->setContentsMargins(0, 0, 0, 0);
    footerRow->setSpacing(8);

    statusLabel = new QLabel(formCard);
    statusLabel->setObjectName(QStringLiteral("conversationStatusLabel"));
    statusLabel->setWordWrap(true);
    statusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    footerRow->addWidget(statusLabel, 1);

    resetBtn = new QPushButton(QStringLiteral("Clear"), formCard);
    resetBtn->setObjectName(QStringLiteral("tagResetButton"));
    resetBtn->setCursor(Qt::PointingHandCursor);
    configureNoAutoDefault(resetBtn);
    QObject::connect(resetBtn, &QPushButton::clicked, owner, [this]() {
        beginCreateMode(formKind, false);
        setStatus({});
    });
    footerRow->addWidget(resetBtn);

    submitBtn = new QPushButton(QStringLiteral("Create Tag"), formCard);
    submitBtn->setObjectName(QStringLiteral("tagSubmitButton"));
    submitBtn->setProperty("primary", true);
    submitBtn->setCursor(Qt::PointingHandCursor);
    configureNoAutoDefault(submitBtn);
    submitBtn->setEnabled(false);
    QObject::connect(submitBtn, &QPushButton::clicked, owner, [this]() {
        submitForm();
    });
    footerRow->addWidget(submitBtn);

    formCardLayout->addLayout(footerRow);

    mainLayout->addWidget(formCard);
    mainLayout->addStretch(1);

    QObject::connect(nameEdit, &QLineEdit::textChanged, owner, [this]() {
        updateTriggerPreview();
        updateSubmitEnabled();
    });
    QObject::connect(descriptionEdit, &QLineEdit::textChanged, owner, [this]() {
        updateTriggerPreview();
        updateSubmitEnabled();
    });
    QObject::connect(directiveEdit, &QPlainTextEdit::textChanged, owner, [this]() {
        updateSubmitEnabled();
    });

    updateFormModeUi();
    rebuildActiveTagsSection();
    rebuildSteeringSection();
    rebuildTagListSection();
}

void ConversationSettingsPane::Impl::rebuildActiveTagsSection() {
    clearLayoutDeferred(activeTagsFlow);

    const bool hasThread = !selectedThreadId.isEmpty();
    int activeEnabledCount = 0;
    int enabledCount = 0;

    for (const RubricTag& tag : tags) {
        if (!tag.enabled) {
            continue;
        }
        ++enabledCount;
        const bool isActive = hasThread && listContainsTag(activeThreadTagIds, tag);
        if (isActive) {
            ++activeEnabledCount;
        }

        const QString displayName = tag.name.trimmed().isEmpty() ? tag.id : tag.name.trimmed();
        const QString chipLabel = tag.id.isEmpty()
            ? displayName
            : QStringLiteral("%1 (@tag:%2)").arg(displayName, tag.id);

        auto* chip = new SpellChipButton(
            chipLabel,
            isActive ? QStringLiteral("check") : QStringLiteral("plus"),
            isActive ? BadgeTone::Accent : BadgeTone::Neutral,
            activeTagsChipHost);
        chip->setCheckable(true);
        chip->setChecked(isActive);
        chip->setEnabled(hasThread);
        configureNoAutoDefault(chip);

        QString tooltip;
        if (!tag.description.trimmed().isEmpty()) {
            tooltip += tag.description.trimmed();
        }
        if (!tag.directive.trimmed().isEmpty()) {
            if (!tooltip.isEmpty()) {
                tooltip += QStringLiteral("\n\n");
            }
            tooltip += tag.directive.trimmed();
        }
        if (!hasThread) {
            if (!tooltip.isEmpty()) {
                tooltip += QStringLiteral("\n\n");
            }
            tooltip += QStringLiteral("Select a conversation thread to toggle this tag.");
        }
        chip->setToolTip(tooltip);

        const RubricTag tagCopy = tag;
        QObject::connect(chip, &QPushButton::clicked, owner, [this, tagCopy]() {
            toggleTagOnSelectedThread(tagCopy);
        });

        activeTagsFlow->addWidget(chip);
    }

    if (!hasThread) {
        activeThreadCaption->setText(
            QStringLiteral("No conversation thread selected. Open or create a conversation to attach rubric tags."));
        activeTagsCountBadge->setText(QStringLiteral("NO THREAD"));
        activeTagsCountBadge->setTone(BadgeTone::Neutral);
        activeTagsCountBadge->setShowDot(false);
        clearActiveTagsBtn->setVisible(false);
    } else {
        activeThreadCaption->setText(
            QStringLiteral("Conversation: %1 — click chips to attach or detach rubric tags for this thread.")
                .arg(selectedThreadTitle.isEmpty() ? selectedThreadId : selectedThreadTitle));
        activeTagsCountBadge->setText(QStringLiteral("%1 ACTIVE").arg(activeEnabledCount));
        activeTagsCountBadge->setTone(activeEnabledCount > 0 ? BadgeTone::Accent : BadgeTone::Neutral);
        activeTagsCountBadge->setShowDot(activeEnabledCount > 0);
        clearActiveTagsBtn->setVisible(!activeThreadTagIds.isEmpty());
    }

    activeTagsChipHost->setVisible(enabledCount > 0);
    activeTagsEmptyLabel->setVisible(enabledCount == 0);
    activeTagsEmptyLabel->setText(
        tags.isEmpty()
            ? QStringLiteral("No rubric tags configured yet.")
            : QStringLiteral("All rubric tags are currently disabled. Enable a tag below to attach it to conversations."));
    activeTagsChipHost->updateGeometry();
    owner->updateGeometry();
}

void ConversationSettingsPane::Impl::rebuildSteeringSection() {
    clearLayoutDeferred(steeringFlow);

    const UiPalette u = ThemeManager::instance().ui();
    const QColor chipGroupBorder = compositeOver(u.divider, u.background);
    const QColor chipGroupBg = u.surface;

    int activeCount = 0;
    for (const SteeringDirectiveItem& item : steeringDirectives) {
        const bool isActive = listContainsSteering(activeSteeringIds, item);
        if (isActive) {
            ++activeCount;
        }

        auto* pairWidget = new QFrame(steeringChipHost);
        pairWidget->setObjectName(QStringLiteral("spellSteeringChipPair"));
        pairWidget->setStyleSheet(QStringLiteral(
            "QFrame#spellSteeringChipPair { background-color: %1; border: 1px solid %2; border-radius: 12px; }")
                                      .arg(qssColor(chipGroupBg), qssColor(chipGroupBorder)));

        auto* pairLayout = new QHBoxLayout(pairWidget);
        pairLayout->setContentsMargins(2, 2, 4, 2);
        pairLayout->setSpacing(2);

        const QString title = item.trigger.isEmpty()
            ? item.name
            : QStringLiteral("%1 (%2)").arg(item.name, item.trigger);

        auto* toggleChip = new SpellChipButton(
            title,
            isActive ? QStringLiteral("check") : QStringLiteral("sparkles"),
            isActive ? BadgeTone::Accent : BadgeTone::Neutral,
            pairWidget);
        toggleChip->setCheckable(true);
        toggleChip->setChecked(isActive);
        toggleChip->setToolTip(item.instruction.isEmpty() ? title : item.instruction);
        configureNoAutoDefault(toggleChip);

        const SteeringDirectiveItem itemCopy = item;
        QObject::connect(toggleChip, &QPushButton::clicked, owner, [this, itemCopy]() {
            toggleSteeringDirective(itemCopy);
        });
        pairLayout->addWidget(toggleChip);

        auto* editBtn = new SpellIconButton(
            QStringLiteral("edit"),
            QStringLiteral("Edit %1 directive").arg(item.name),
            BadgeTone::Neutral,
            pairWidget);
        editBtn->setIconPixelSize(12);
        QObject::connect(editBtn, &QToolButton::clicked, owner, [this, itemCopy]() {
            beginEditSteering(itemCopy);
        });
        pairLayout->addWidget(editBtn);

        steeringFlow->addWidget(pairWidget);
    }

    if (activeCount > 0) {
        steeringCountBadge->setText(QStringLiteral("%1 ACTIVE").arg(activeCount));
        steeringCountBadge->setTone(BadgeTone::Accent);
        steeringCountBadge->setShowDot(true);
    } else {
        steeringCountBadge->setText(QStringLiteral("%1 DIRECTIVES").arg(steeringDirectives.size()));
        steeringCountBadge->setTone(BadgeTone::Neutral);
        steeringCountBadge->setShowDot(false);
    }
    steeringChipHost->updateGeometry();
    owner->updateGeometry();
}

void ConversationSettingsPane::Impl::rebuildTagListSection() {
    clearLayoutDeferred(tagListLayout);

    const UiPalette u = ThemeManager::instance().ui();
    const QColor rowBg = compositeOver(u.elevatedSurface, u.background);
    const QColor rowBorder = compositeOver(u.divider, u.background);
    const QColor editingBorder = u.selectedBorder;
    const QColor primaryColor = u.textPrimary;
    const QColor secondaryColor = compositeOver(u.textSecondary, u.background);
    const QColor mutedColor = compositeOver(u.textMuted, u.background);

    QFont monoFont(ThemeManager::instance().editorFontFamily());
    monoFont.setStyleHint(QFont::Monospace);
    monoFont.setPointSizeF(10.5);

    const QString filterText = tagFilterEdit ? tagFilterEdit->text().trimmed() : QString();
    int shownCount = 0;

    for (const RubricTag& tag : tags) {
        if (!filterText.isEmpty()) {
            const bool matches =
                tag.name.contains(filterText, Qt::CaseInsensitive) ||
                tag.id.contains(filterText, Qt::CaseInsensitive) ||
                tag.description.contains(filterText, Qt::CaseInsensitive) ||
                tag.directive.contains(filterText, Qt::CaseInsensitive);
            if (!matches) {
                continue;
            }
        }
        ++shownCount;

        const bool isEditingThis = (formKind == FormKind::Tag && !editingItemId.isEmpty() && editingItemId == tag.id);
        const bool isActiveOnThread = !selectedThreadId.isEmpty() && listContainsTag(activeThreadTagIds, tag);

        auto* rowFrame = new QFrame(tagListHost);
        rowFrame->setObjectName(QStringLiteral("spellTagRowCard"));
        rowFrame->setProperty("editing", isEditingThis);
        rowFrame->setStyleSheet(QStringLiteral(
            "QFrame#spellTagRowCard { background-color: %1; border: 1px solid %2; border-radius: 7px; }")
                                    .arg(qssColor(rowBg),
                                         qssColor(isEditingThis ? editingBorder : rowBorder)));
        if (!tag.directive.trimmed().isEmpty()) {
            rowFrame->setToolTip(tag.directive.trimmed());
        }

        auto* rowLayout = new QHBoxLayout(rowFrame);
        rowLayout->setContentsMargins(10, 9, 10, 9);
        rowLayout->setSpacing(10);

        // 1. Enable checkbox
        auto* enableCheck = new QCheckBox(rowFrame);
        enableCheck->setObjectName(QStringLiteral("tagRowEnableCheck"));
        enableCheck->setChecked(tag.enabled);
        enableCheck->setCursor(Qt::PointingHandCursor);
        enableCheck->setToolTip(tag.enabled
                                    ? QStringLiteral("Disable \"%1\" rubric tag").arg(tag.name)
                                    : QStringLiteral("Enable \"%1\" rubric tag").arg(tag.name));
        const RubricTag tagCopy = tag;
        QObject::connect(enableCheck, &QCheckBox::clicked, owner, [this, tagCopy](bool checked) {
            toggleTagEnabled(tagCopy, checked);
        });
        rowLayout->addWidget(enableCheck, 0, Qt::AlignTop);

        // 2. Center column: Name + @tag:id + badges + description + directive preview
        auto* infoCol = new QVBoxLayout();
        infoCol->setContentsMargins(0, 0, 0, 0);
        infoCol->setSpacing(3);

        auto* titleRow = new QHBoxLayout();
        titleRow->setContentsMargins(0, 0, 0, 0);
        titleRow->setSpacing(6);

        const QString displayName = tag.name.trimmed().isEmpty() ? tag.id : tag.name.trimmed();
        auto* titleLabel = new QLabel(displayName, rowFrame);
        titleLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12.5px; font-weight: 600;")
                                      .arg(qssColor(tag.enabled ? primaryColor : mutedColor)));
        titleRow->addWidget(titleLabel);

        if (!tag.id.isEmpty()) {
            auto* idLabel = new QLabel(QStringLiteral("@tag:%1").arg(tag.id), rowFrame);
            idLabel->setFont(monoFont);
            idLabel->setStyleSheet(QStringLiteral("color: %1;").arg(qssColor(mutedColor)));
            titleRow->addWidget(idLabel);
        }

        if (tag.isBuiltIn) {
            auto* builtInBadge = new SpellBadge(QStringLiteral("BUILT-IN"), BadgeTone::Neutral, false, rowFrame);
            builtInBadge->setToolTip(QStringLiteral("Shipped built-in BeatrixLeslie IDE rubric tag"));
            titleRow->addWidget(builtInBadge);
        }

        if (isActiveOnThread) {
            auto* activeBadge = new SpellBadge(QStringLiteral("ACTIVE"), BadgeTone::Accent, true, rowFrame);
            activeBadge->setToolTip(QStringLiteral("Attached to the selected conversation thread"));
            titleRow->addWidget(activeBadge);
        }

        if (!tag.enabled) {
            auto* disabledBadge = new SpellBadge(QStringLiteral("DISABLED"), BadgeTone::Neutral, false, rowFrame);
            titleRow->addWidget(disabledBadge);
        }

        titleRow->addStretch(1);
        infoCol->addLayout(titleRow);

        if (!tag.description.trimmed().isEmpty()) {
            auto* descLabel = new QLabel(tag.description.trimmed(), rowFrame);
            descLabel->setWordWrap(true);
            descLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11.5px;")
                                         .arg(qssColor(tag.enabled ? secondaryColor : mutedColor)));
            infoCol->addWidget(descLabel);
        }

        if (!tag.directive.trimmed().isEmpty()) {
            auto* dirPreview = new QLabel(compactPreviewText(tag.directive), rowFrame);
            dirPreview->setFont(monoFont);
            dirPreview->setWordWrap(true);
            dirPreview->setStyleSheet(QStringLiteral("color: %1;").arg(qssColor(mutedColor)));
            infoCol->addWidget(dirPreview);
        }

        rowLayout->addLayout(infoCol, 1);

        // 3. Action buttons: Edit & Delete
        auto* actionsCol = new QHBoxLayout();
        actionsCol->setContentsMargins(0, 0, 0, 0);
        actionsCol->setSpacing(4);
        actionsCol->setAlignment(Qt::AlignTop | Qt::AlignRight);

        auto* editBtn = new SpellIconButton(
            QStringLiteral("edit"),
            QStringLiteral("Edit \"%1\"").arg(displayName),
            BadgeTone::Neutral,
            rowFrame);
        editBtn->setObjectName(QStringLiteral("tagRowEditButton"));
        QObject::connect(editBtn, &QToolButton::clicked, owner, [this, tagCopy]() {
            beginEditTag(tagCopy);
        });
        actionsCol->addWidget(editBtn, 0, Qt::AlignTop);

        auto* deleteBtn = new SpellIconButton(
            QStringLiteral("trash"),
            tag.isBuiltIn
                ? QStringLiteral("Built-in tags cannot be deleted")
                : QStringLiteral("Delete \"%1\"").arg(displayName),
            BadgeTone::Danger,
            rowFrame);
        deleteBtn->setObjectName(QStringLiteral("tagRowDeleteButton"));
        deleteBtn->setEnabled(!tag.isBuiltIn);
        QObject::connect(deleteBtn, &QToolButton::clicked, owner, [this, tagCopy]() {
            deleteRubricTag(tagCopy);
        });
        actionsCol->addWidget(deleteBtn, 0, Qt::AlignTop);

        rowLayout->addLayout(actionsCol, 0);

        tagListLayout->addWidget(rowFrame);
    }

    const int totalTags = static_cast<int>(tags.size());
    registryCountBadge->setText(totalTags == 1
                                    ? QStringLiteral("1 TAG")
                                    : QStringLiteral("%1 TAGS").arg(totalTags));
    registryCountBadge->setTone(totalTags > 0 ? BadgeTone::Info : BadgeTone::Neutral);

    tagListHost->setVisible(shownCount > 0);
    tagListEmptyLabel->setVisible(shownCount == 0);
    if (tags.isEmpty()) {
        tagListEmptyLabel->setText(
            QStringLiteral("No rubric tags configured yet. Use the form below to create a custom tag."));
    } else if (shownCount == 0) {
        tagListEmptyLabel->setText(
            QStringLiteral("No rubric tags match \"%1\".").arg(filterText));
    }
    tagListHost->updateGeometry();
    owner->updateGeometry();
}

void ConversationSettingsPane::Impl::updateFormModeUi() {
    const bool isTag = (formKind == FormKind::Tag);
    const bool isEditing = !editingItemId.isEmpty();

    kindTagBtn->setChecked(isTag);
    kindSteeringBtn->setChecked(!isTag);

    if (isTag) {
        formHeader->setTitle(isEditing ? QStringLiteral("Edit Rubric Tag") : QStringLiteral("Add Rubric Tag"));
        nameLabel->setText(QStringLiteral("Name"));
        nameEdit->setPlaceholderText(QStringLiteral("e.g. Security Review"));
        descriptionLabel->setText(QStringLiteral("Description"));
        descriptionEdit->setPlaceholderText(QStringLiteral("Short summary of when to apply this tag"));
        directiveLabel->setText(QStringLiteral("Directive"));
        directiveEdit->setPlaceholderText(
            QStringLiteral("Enter the rubric instructions or evaluation rules injected when this tag is active…"));
        formEnabledCheck->setVisible(true);
        formEnabledCheck->setEnabled(isEditing);
        formEnabledCheck->setToolTip(
            isEditing
                ? QStringLiteral("Enable or disable this rubric tag")
                : QStringLiteral("New rubric tags start enabled; toggle enabled state after creating"));
        submitBtn->setText(isEditing ? QStringLiteral("Save Changes") : QStringLiteral("Create Tag"));
    } else {
        formHeader->setTitle(isEditing
                                 ? QStringLiteral("Edit Steering Directive")
                                 : QStringLiteral("Add Steering Directive"));
        nameLabel->setText(QStringLiteral("Name"));
        nameEdit->setPlaceholderText(QStringLiteral("e.g. Strict Diffs"));
        descriptionLabel->setText(QStringLiteral("Trigger Slug (optional)"));
        descriptionEdit->setPlaceholderText(QStringLiteral("e.g. strict-diffs (triggers as #steer:strict-diffs)"));
        directiveLabel->setText(QStringLiteral("Directive"));
        directiveEdit->setPlaceholderText(
            QStringLiteral("Enter per-turn steering instructions appended when this directive is toggled on…"));
        formEnabledCheck->setVisible(false);
        submitBtn->setText(isEditing ? QStringLiteral("Save Directive") : QStringLiteral("Create Directive"));
    }

    if (isEditing) {
        const QString prefix = isTag ? QStringLiteral("@tag:") : QStringLiteral("#steer:");
        editingBadge->setText(QStringLiteral("EDITING · %1%2").arg(prefix, editingItemId));
        editingBadge->setVisible(true);
        resetBtn->setText(QStringLiteral("Cancel Edit"));
    } else {
        editingBadge->setVisible(false);
        resetBtn->setText(QStringLiteral("Clear"));
    }

    updateTriggerPreview();
    updateSubmitEnabled();
}

void ConversationSettingsPane::Impl::updateTriggerPreview() {
    if (formKind == FormKind::Tag) {
        const QString slug = !editingItemId.isEmpty()
            ? editingItemId
            : slugifyIdentifier(nameEdit->text());
        if (slug.isEmpty()) {
            triggerPreviewLabel->setText(QStringLiteral("Inline trigger: @tag:<slug>"));
        } else {
            triggerPreviewLabel->setText(QStringLiteral("Inline trigger: @tag:%1").arg(slug));
        }
    } else {
        QString triggerText;
        if (!editingItemId.isEmpty()) {
            for (const SteeringDirectiveItem& item : steeringDirectives) {
                if (item.id.compare(editingItemId, Qt::CaseInsensitive) == 0 && !item.trigger.isEmpty()) {
                    triggerText = item.trigger;
                    break;
                }
            }
            if (triggerText.isEmpty()) {
                triggerText = QStringLiteral("#steer:") + editingItemId;
            }
        } else {
            const QString rawSlug = descriptionEdit->text().trimmed().isEmpty()
                ? slugifyIdentifier(nameEdit->text())
                : slugifyIdentifier(descriptionEdit->text());
            if (!rawSlug.isEmpty()) {
                triggerText = QStringLiteral("#steer:") + rawSlug;
            }
        }
        if (triggerText.isEmpty()) {
            triggerPreviewLabel->setText(QStringLiteral("Inline trigger: #steer:<slug>"));
        } else {
            triggerPreviewLabel->setText(QStringLiteral("Inline trigger: %1").arg(triggerText));
        }
    }
}

void ConversationSettingsPane::Impl::updateSubmitEnabled() {
    const bool hasName = !nameEdit->text().trimmed().isEmpty();
    submitBtn->setEnabled(hasName);
}

void ConversationSettingsPane::Impl::setStatus(const QString& text, bool isError) {
    statusIsError = isError;
    statusLabel->setText(text);
    const UiPalette u = ThemeManager::instance().ui();
    const QColor color = isError ? u.danger : compositeOver(u.textSecondary, u.background);
    statusLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(qssColor(color)));
}

void ConversationSettingsPane::Impl::beginCreateMode(FormKind kind, bool focusNameField) {
    formKind = kind;
    editingItemId.clear();
    nameEdit->clear();
    descriptionEdit->clear();
    directiveEdit->clear();
    formEnabledCheck->setChecked(true);
    updateFormModeUi();
    rebuildTagListSection();
    if (focusNameField) {
        nameEdit->setFocus();
    }
}

void ConversationSettingsPane::Impl::beginEditTag(const RubricTag& tag) {
    formKind = FormKind::Tag;
    editingItemId = tag.id.isEmpty() ? tag.name : tag.id;
    nameEdit->setText(tag.name);
    descriptionEdit->setText(tag.description);
    directiveEdit->setPlainText(tag.directive);
    formEnabledCheck->setChecked(tag.enabled);
    updateFormModeUi();
    rebuildTagListSection();
    setStatus(QStringLiteral("Editing rubric tag \"%1\".").arg(tag.name));
    ensureVisibleInAncestorScrollArea(formCard, 0, 24);
    nameEdit->setFocus();
    nameEdit->selectAll();
}

void ConversationSettingsPane::Impl::beginEditSteering(const SteeringDirectiveItem& item) {
    formKind = FormKind::Steering;
    editingItemId = item.id;
    nameEdit->setText(item.name);
    descriptionEdit->setText(item.id);
    directiveEdit->setPlainText(item.instruction);
    updateFormModeUi();
    rebuildTagListSection();
    setStatus(QStringLiteral("Editing steering directive \"%1\".").arg(item.name));
    ensureVisibleInAncestorScrollArea(formCard, 0, 24);
    nameEdit->setFocus();
    nameEdit->selectAll();
}

void ConversationSettingsPane::Impl::submitForm() {
    const QString name = nameEdit->text().trimmed();
    if (name.isEmpty()) {
        setStatus(QStringLiteral("Please enter a name before saving."), true);
        nameEdit->setFocus();
        return;
    }

    const QString description = descriptionEdit->text().trimmed();
    const QString directive = directiveEdit->toPlainText().trimmed();
    const QString effectiveDirective = directive.isEmpty()
        ? QStringLiteral("Apply %1 instructions.").arg(name)
        : directive;

    if (formKind == FormKind::Tag) {
        if (editingItemId.isEmpty()) {
            if (client) {
                client->createTag(name, description, effectiveDirective);
            }
            setStatus(QStringLiteral("Created rubric tag \"%1\".").arg(name));
        } else {
            const QString id = editingItemId;
            const bool enabled = formEnabledCheck->isChecked();
            if (client) {
                client->updateTag(id, name, description, effectiveDirective, enabled);
            }
            for (RubricTag& existing : tags) {
                if (existing.id == id) {
                    existing.name = name;
                    existing.description = description;
                    existing.directive = effectiveDirective;
                    existing.enabled = enabled;
                    break;
                }
            }
            setStatus(QStringLiteral("Saved changes to rubric tag \"%1\".").arg(name));
        }
        beginCreateMode(FormKind::Tag, false);
        rebuildActiveTagsSection();
        rebuildTagListSection();
    } else {
        QString steeringId = editingItemId;
        if (steeringId.isEmpty()) {
            steeringId = slugifyIdentifier(description.isEmpty() ? name : description);
            if (steeringId.isEmpty()) {
                steeringId = slugifyIdentifier(name);
            }
        }
        if (client && !steeringId.isEmpty()) {
            const QJsonObject cmd{
                {QStringLiteral("cmd"), QStringLiteral("upsert_steering")},
                {QStringLiteral("id"), steeringId},
                {QStringLiteral("name"), name},
                {QStringLiteral("instruction"), effectiveDirective},
            };
            client->dispatchCommand(cmd);
        }

        bool updatedLocal = false;
        for (SteeringDirectiveItem& existing : steeringDirectives) {
            if (existing.id.compare(steeringId, Qt::CaseInsensitive) == 0) {
                existing.name = name;
                existing.instruction = effectiveDirective;
                updatedLocal = true;
                break;
            }
        }
        if (!updatedLocal && !steeringId.isEmpty()) {
            steeringDirectives.append(SteeringDirectiveItem{
                steeringId,
                name,
                QStringLiteral("#steer:") + steeringId,
                effectiveDirective,
                false,
            });
        }

        const bool wasEdit = !editingItemId.isEmpty();
        setStatus(wasEdit
                      ? QStringLiteral("Updated steering directive \"%1\".").arg(name)
                      : QStringLiteral("Created steering directive \"%1\".").arg(name));
        beginCreateMode(FormKind::Steering, false);
        rebuildSteeringSection();
    }
}

void ConversationSettingsPane::Impl::toggleTagOnSelectedThread(const RubricTag& tag) {
    if (selectedThreadId.isEmpty()) {
        setStatus(QStringLiteral("Select a conversation thread first to attach tags."), true);
        return;
    }

    QStringList nextTagIds;
    bool removed = false;
    for (const QString& existing : activeThreadTagIds) {
        const bool matchesId = !tag.id.isEmpty() && existing.compare(tag.id, Qt::CaseInsensitive) == 0;
        const bool matchesName = !tag.name.isEmpty() && existing.compare(tag.name, Qt::CaseInsensitive) == 0;
        if (matchesId || matchesName) {
            removed = true;
            continue;
        }
        nextTagIds.append(existing);
    }
    if (!removed) {
        const QString canonicalId = tag.id.isEmpty() ? tag.name : tag.id;
        if (!canonicalId.isEmpty()) {
            nextTagIds.append(canonicalId);
        }
    }

    activeThreadTagIds = nextTagIds;
    if (client) {
        client->setThreadTags(selectedThreadId, nextTagIds);
    }

    setStatus(removed
                  ? QStringLiteral("Detached \"%1\" from active conversation.").arg(tag.name)
                  : QStringLiteral("Attached \"%1\" to active conversation.").arg(tag.name));
    rebuildActiveTagsSection();
    rebuildTagListSection();
}

void ConversationSettingsPane::Impl::clearSelectedThreadTags() {
    if (selectedThreadId.isEmpty() || activeThreadTagIds.isEmpty()) {
        return;
    }
    activeThreadTagIds.clear();
    if (client) {
        client->setThreadTags(selectedThreadId, {});
    }
    setStatus(QStringLiteral("Cleared active conversation rubric tags."));
    rebuildActiveTagsSection();
    rebuildTagListSection();
}

void ConversationSettingsPane::Impl::toggleTagEnabled(const RubricTag& tag, bool enabled) {
    if (client) {
        client->updateTag(tag.id, tag.name, tag.description, tag.directive, enabled);
    }
    for (RubricTag& existing : tags) {
        if (existing.id == tag.id) {
            existing.enabled = enabled;
            break;
        }
    }
    if (formKind == FormKind::Tag && editingItemId == tag.id) {
        formEnabledCheck->setChecked(enabled);
    }
    setStatus(enabled
                  ? QStringLiteral("Enabled rubric tag \"%1\".").arg(tag.name)
                  : QStringLiteral("Disabled rubric tag \"%1\".").arg(tag.name));
    rebuildActiveTagsSection();
    rebuildTagListSection();
}

void ConversationSettingsPane::Impl::deleteRubricTag(const RubricTag& tag) {
    if (tag.isBuiltIn) {
        setStatus(QStringLiteral("Built-in rubric tags cannot be deleted."), true);
        return;
    }

    if (client) {
        client->deleteTag(tag.id);
    }

    tags.erase(
        std::remove_if(tags.begin(), tags.end(), [&](const RubricTag& t) { return t.id == tag.id; }),
        tags.end());
    activeThreadTagIds.erase(
        std::remove_if(activeThreadTagIds.begin(),
                       activeThreadTagIds.end(),
                       [&](const QString& entry) {
                           return entry.compare(tag.id, Qt::CaseInsensitive) == 0 ||
                                  entry.compare(tag.name, Qt::CaseInsensitive) == 0;
                       }),
        activeThreadTagIds.end());

    if (formKind == FormKind::Tag && editingItemId == tag.id) {
        editingItemId.clear();
        nameEdit->clear();
        descriptionEdit->clear();
        directiveEdit->clear();
        formEnabledCheck->setChecked(true);
        updateFormModeUi();
    }

    setStatus(QStringLiteral("Deleted rubric tag \"%1\".").arg(tag.name));
    rebuildActiveTagsSection();
    rebuildTagListSection();
}

void ConversationSettingsPane::Impl::toggleSteeringDirective(const SteeringDirectiveItem& item) {
    QStringList nextIds;
    bool removed = false;
    for (const QString& existing : activeSteeringIds) {
        if (existing.compare(item.id, Qt::CaseInsensitive) == 0 ||
            (!item.trigger.isEmpty() && existing.compare(item.trigger, Qt::CaseInsensitive) == 0)) {
            removed = true;
            continue;
        }
        nextIds.append(existing);
    }
    if (!removed) {
        nextIds.append(item.id);
    }
    activeSteeringIds = nextIds;

    if (client) {
        const QJsonObject cmd{
            {QStringLiteral("cmd"), QStringLiteral("toggle_steering")},
            {QStringLiteral("steering_id"), item.id},
        };
        client->dispatchCommand(cmd);
    }

    setStatus(removed
                  ? QStringLiteral("Disabled steering directive \"%1\".").arg(item.name)
                  : QStringLiteral("Enabled steering directive \"%1\".").arg(item.name));
    rebuildSteeringSection();
}

void ConversationSettingsPane::Impl::applyTheme() {
    const UiPalette u = ThemeManager::instance().ui();
    const QColor borderColor = compositeOver(u.subtleBorder, u.background);
    const QColor secondaryColor = compositeOver(u.textSecondary, u.background);
    const QColor mutedColor = compositeOver(u.textMuted, u.background);
    const QColor selectedBg = compositeOver(u.selectedFill, u.background);

    owner->setStyleSheet(QStringLiteral(
        "QWidget#conversationSettingsPane {\n"
        "  background-color: %1;\n"
        "  color: %2;\n"
        "  border: none;\n"
        "}\n"
        "QWidget#spellTransparentHost {\n"
        "  background-color: transparent;\n"
        "}\n"
        "QFrame#spellSettingsCard {\n"
        "  background-color: %3;\n"
        "  border: 1px solid %4;\n"
        "  border-radius: 8px;\n"
        "}\n"
        "QFrame#spellSettingsCard QLabel {\n"
        "  background-color: transparent;\n"
        "}\n")
                             .arg(qssColor(u.background),
                                  qssColor(u.textPrimary),
                                  qssColor(u.surface),
                                  qssColor(borderColor)));

    const QString captionStyle = QStringLiteral("color: %1; font-size: 11.5px;").arg(qssColor(secondaryColor));
    const QString mutedStyle = QStringLiteral("color: %1; font-size: 11px;").arg(qssColor(mutedColor));
    const QString fieldLabelStyle = QStringLiteral("color: %1; font-size: 11.5px; font-weight: 600;")
                                        .arg(qssColor(secondaryColor));

    introCaption->setStyleSheet(captionStyle);
    activeThreadCaption->setStyleSheet(captionStyle);
    activeTagsEmptyLabel->setStyleSheet(mutedStyle);
    steeringHintLabel->setStyleSheet(captionStyle);
    tagListEmptyLabel->setStyleSheet(mutedStyle);

    nameLabel->setStyleSheet(fieldLabelStyle);
    descriptionLabel->setStyleSheet(fieldLabelStyle);
    directiveLabel->setStyleSheet(fieldLabelStyle);

    QFont monoFont(ThemeManager::instance().editorFontFamily());
    monoFont.setStyleHint(QFont::Monospace);
    monoFont.setPointSizeF(11.0);
    directiveEdit->setFont(monoFont);

    QFont previewFont = monoFont;
    previewFont.setPointSizeF(10.0);
    triggerPreviewLabel->setFont(previewFont);
    triggerPreviewLabel->setStyleSheet(mutedStyle);

    const QString kindBtnStyle = QStringLiteral(
        "QPushButton {\n"
        "  background-color: transparent;\n"
        "  color: %1;\n"
        "  border: 1px solid %2;\n"
        "  border-radius: 5px;\n"
        "  padding: 3px 9px;\n"
        "  font-size: 11px;\n"
        "  font-weight: 600;\n"
        "}\n"
        "QPushButton:checked {\n"
        "  background-color: %3;\n"
        "  color: %4;\n"
        "  border-color: %5;\n"
        "}\n")
                                     .arg(qssColor(secondaryColor),
                                          qssColor(borderColor),
                                          qssColor(selectedBg),
                                          qssColor(u.textPrimary),
                                          qssColor(u.selectedBorder));
    kindTagBtn->setStyleSheet(kindBtnStyle);
    kindSteeringBtn->setStyleSheet(kindBtnStyle);

    newTagBtn->setIcon(icons::vectorIcon(QStringLiteral("plus"), u.textPrimary, 13));
    submitBtn->setIcon(icons::vectorIcon(QStringLiteral("check"), u.textPrimary, 13));
    clearActiveTagsBtn->setIcon(icons::vectorIcon(QStringLiteral("close"), secondaryColor, 12));

    setStatus(statusLabel->text(), statusIsError);

    // Re-apply theme to all custom Spell widgets and dynamic rows
    for (SpellSectionHeader* header : owner->findChildren<SpellSectionHeader*>()) {
        header->applyTheme();
    }
    for (SpellBadge* badge : owner->findChildren<SpellBadge*>()) {
        badge->applyTheme();
    }
    for (SpellChipButton* chip : owner->findChildren<SpellChipButton*>()) {
        chip->applyTheme();
    }
    for (SpellIconButton* iconBtn : owner->findChildren<SpellIconButton*>()) {
        iconBtn->applyTheme();
    }
    for (SpellHairline* line : owner->findChildren<SpellHairline*>()) {
        line->applyTheme();
    }

    rebuildSteeringSection();
    rebuildTagListSection();
}

ConversationSettingsPane::ConversationSettingsPane(SpellCoreClient* client, QWidget* parent)
    : QWidget(parent)
    , m_impl(std::make_unique<Impl>()) {
    m_impl->owner = this;
    m_impl->client = client;
    m_impl->setupUi();
    m_impl->applyTheme();

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        applyTheme();
    });

    if (m_impl->client) {
        connect(m_impl->client,
                &SpellCoreClient::snapshotUpdated,
                this,
                [this](const WorkspaceSnapshot& snap) {
                    updateSnapshot(snap);
                });
        connect(m_impl->client,
                &SpellCoreClient::errorOccurred,
                this,
                [this](const QString& message) {
                    if (!message.trimmed().isEmpty()) {
                        m_impl->setStatus(message.trimmed(), true);
                    }
                });
        updateSnapshot(m_impl->client->currentSnapshot());
    }
}

ConversationSettingsPane::~ConversationSettingsPane() = default;

void ConversationSettingsPane::updateSnapshot(const WorkspaceSnapshot& snap) {
    const QString nextThreadId = snap.selectedThreadId;
    const QString nextThreadTitle = resolveThreadTitle(snap);
    const QVector<SteeringDirectiveItem> nextSteering = parseSteeringDirectives(snap.raw);
    const QStringList nextActiveSteering = parseActiveSteeringIds(snap.raw);

    const bool threadChanged =
        (nextThreadId != m_impl->selectedThreadId) || (nextThreadTitle != m_impl->selectedThreadTitle);
    const bool tagsChanged = (snap.tags != m_impl->tags);
    const bool activeThreadTagsChanged = (snap.activeThreadTagIds != m_impl->activeThreadTagIds);
    const bool steeringChanged = (nextSteering != m_impl->steeringDirectives);
    const bool activeSteeringChanged = (nextActiveSteering != m_impl->activeSteeringIds);

    const bool firstSnapshot = !m_impl->hasSnapshot;
    m_impl->hasSnapshot = true;
    m_impl->selectedThreadId = nextThreadId;
    m_impl->selectedThreadTitle = nextThreadTitle;
    m_impl->tags = snap.tags;
    m_impl->activeThreadTagIds = snap.activeThreadTagIds;
    m_impl->steeringDirectives = nextSteering;
    m_impl->activeSteeringIds = nextActiveSteering;

    if (firstSnapshot || threadChanged || tagsChanged || activeThreadTagsChanged) {
        m_impl->rebuildActiveTagsSection();
    }
    if (firstSnapshot || steeringChanged || activeSteeringChanged) {
        m_impl->rebuildSteeringSection();
    }
    if (firstSnapshot || tagsChanged || activeThreadTagsChanged || threadChanged) {
        m_impl->rebuildTagListSection();
    }
}

void ConversationSettingsPane::applyTheme() {
    m_impl->applyTheme();
}

} // namespace spell
