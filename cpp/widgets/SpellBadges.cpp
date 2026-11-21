#include "widgets/SpellBadges.h"

#include "theme/SpellIcons.h"

#include <QEnterEvent>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QRectF>
#include <QSizePolicy>
#include <QtMath>

#include <algorithm>

namespace spell {

namespace {

constexpr int kBadgePadH = 7;
constexpr int kBadgePadV = 3;
constexpr int kBadgeDotSize = 6;
constexpr int kBadgeDotSpacing = 4;
constexpr int kBadgeMinHeight = 18;

constexpr int kChipIconSize = 12;
constexpr int kIconBtnMinWidth = 24;
constexpr int kIconBtnMinHeight = 22;
constexpr int kSectionIconSize = 12;

QColor scaledAlpha(const QColor& color, double factor) {
    const double clampedFactor = std::clamp(factor, 0.0, 1.0);
    return withAlpha(color, color.alphaF() * clampedFactor);
}

QFont makeMonoFont(double pointSize, QFont::Weight weight) {
    QFont font(ThemeManager::instance().editorFontFamily());
    font.setStyleHint(QFont::Monospace);
    font.setPointSizeF(pointSize);
    font.setWeight(weight);
    return font;
}

QFont makeUiFont(double pointSize, QFont::Weight weight) {
    QFont font = QGuiApplication::font();
    font.setPointSizeF(pointSize);
    font.setWeight(weight);
    return font;
}

} // namespace

// ---------------------------------------------------------------------------
// BadgeTone color resolution
// ---------------------------------------------------------------------------

QColor badgeToneColor(BadgeTone tone, const UiPalette& palette) {
    switch (tone) {
    case BadgeTone::Accent:  return palette.accent;
    case BadgeTone::Success: return palette.success;
    case BadgeTone::Warning: return palette.warning;
    case BadgeTone::Danger:  return palette.danger;
    case BadgeTone::Info:    return palette.info;
    case BadgeTone::Neutral: return palette.textSecondary;
    }
    return palette.textSecondary;
}

QColor badgeToneColor(BadgeTone tone) {
    return badgeToneColor(tone, ThemeManager::instance().ui());
}

// ---------------------------------------------------------------------------
// SpellBadge
// ---------------------------------------------------------------------------

SpellBadge::SpellBadge(QWidget* parent)
    : SpellBadge(QString(), BadgeTone::Neutral, false, parent) {}

SpellBadge::SpellBadge(const QString& text, QWidget* parent)
    : SpellBadge(text, BadgeTone::Neutral, false, parent) {}

SpellBadge::SpellBadge(const QString& text, BadgeTone tone, QWidget* parent)
    : SpellBadge(text, tone, false, parent) {}

SpellBadge::SpellBadge(const QString& text, BadgeTone tone, bool showDot, QWidget* parent)
    : QLabel(text, parent)
    , m_tone(tone)
    , m_showDot(showDot)
    , m_filled(true) {
    initDefaults();
}

void SpellBadge::initDefaults() {
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    setAutoFillBackground(false);
    setWordWrap(false);

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SpellBadge::applyTheme);

    applyTheme();
}

void SpellBadge::updateMetrics() {
    updateGeometry();
}

void SpellBadge::setText(const QString& text) {
    if (QLabel::text() == text) {
        return;
    }
    QLabel::setText(text);
    updateMetrics();
    update();
}

void SpellBadge::setTone(BadgeTone tone) {
    if (m_tone == tone) {
        return;
    }
    m_tone = tone;
    applyTheme();
}

void SpellBadge::setShowDot(bool showDot) {
    if (m_showDot == showDot) {
        return;
    }
    m_showDot = showDot;
    updateMetrics();
    update();
}

void SpellBadge::setFilled(bool filled) {
    if (m_filled == filled) {
        return;
    }
    m_filled = filled;
    update();
}

QSize SpellBadge::sizeHint() const {
    const QFontMetrics fm(font());
    const QString labelText = text();
    const int textWidth = labelText.isEmpty()
        ? 0
        : std::max(fm.horizontalAdvance(labelText), fm.boundingRect(labelText).width());
    const int textHeight = fm.height();

    int contentWidth = textWidth;
    if (m_showDot) {
        contentWidth += kBadgeDotSize;
        if (!labelText.isEmpty()) {
            contentWidth += kBadgeDotSpacing;
        }
    }
    const int contentHeight = std::max(textHeight, m_showDot ? kBadgeDotSize : 0);

    // Reserve 2px total (1px per side) for the crisp 1px capsule hairline stroke.
    const int width = contentWidth + (2 * kBadgePadH) + 2;
    const int height = std::max(kBadgeMinHeight, contentHeight + (2 * kBadgePadV) + 2);
    return QSize(width, height);
}

QSize SpellBadge::minimumSizeHint() const {
    return sizeHint();
}

void SpellBadge::applyTheme() {
    setFont(makeMonoFont(9.5, QFont::Bold));

    const UiPalette palette = ThemeManager::instance().ui();
    const QColor fg = m_showDot ? palette.textPrimary : badgeToneColor(m_tone, palette);
    QPalette pal = this->palette();
    pal.setColor(QPalette::WindowText, fg);
    pal.setColor(QPalette::Text, fg);
    setPalette(pal);

    updateMetrics();
    update();
}

void SpellBadge::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    const UiPalette palette = ThemeManager::instance().ui();
    QColor tone = badgeToneColor(m_tone, palette);
    QColor textColor = m_showDot ? palette.textPrimary : tone;
    if (!isEnabled()) {
        tone = scaledAlpha(tone, 0.45);
        textColor = scaledAlpha(textColor, 0.45);
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QRectF bounds = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (bounds.width() <= 0.0 || bounds.height() <= 0.0) {
        return;
    }

    const qreal radius = bounds.height() / 2.0;
    painter.setBrush(m_filled ? QBrush(scaledAlpha(tone, 0.16)) : Qt::NoBrush);
    painter.setPen(QPen(scaledAlpha(tone, 0.50), 1.0));
    painter.drawRoundedRect(bounds, radius, radius);

    const QString labelText = text();
    const QFontMetrics fm(font());
    const int textWidth = labelText.isEmpty()
        ? 0
        : std::max(fm.horizontalAdvance(labelText), fm.boundingRect(labelText).width());

    int totalContentWidth = textWidth;
    if (m_showDot) {
        totalContentWidth += kBadgeDotSize;
        if (!labelText.isEmpty()) {
            totalContentWidth += kBadgeDotSpacing;
        }
    }

    const qreal innerInset = kBadgePadH + 0.5;
    const qreal innerLeft = bounds.left() + innerInset;
    const qreal innerRight = bounds.right() - innerInset;
    const qreal innerWidth = std::max<qreal>(0.0, innerRight - innerLeft);

    qreal cursorX = innerLeft;
    if (totalContentWidth < innerWidth) {
        cursorX = innerLeft + (innerWidth - totalContentWidth) / 2.0;
    }

    if (m_showDot) {
        const qreal dotY = bounds.center().y() - (kBadgeDotSize / 2.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(tone);
        painter.drawEllipse(QRectF(cursorX, dotY, kBadgeDotSize, kBadgeDotSize));
        cursorX += kBadgeDotSize + kBadgeDotSpacing;
    }

    if (!labelText.isEmpty()) {
        const qreal textAvailWidth = std::max<qreal>(0.0, innerRight - cursorX);
        const QRectF textRect(cursorX, bounds.top(), textAvailWidth, bounds.height());
        const QString drawText = (textWidth > textAvailWidth + 0.5)
            ? fm.elidedText(labelText, Qt::ElideRight, std::max(0, qFloor(textAvailWidth)))
            : labelText;

        painter.setFont(font());
        painter.setPen(textColor);
        painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, drawText);
    }
}

// ---------------------------------------------------------------------------
// SpellChipButton
// ---------------------------------------------------------------------------

SpellChipButton::SpellChipButton(QWidget* parent)
    : SpellChipButton(QString(), QString(), BadgeTone::Accent, parent) {}

SpellChipButton::SpellChipButton(const QString& text, QWidget* parent)
    : SpellChipButton(text, QString(), BadgeTone::Accent, parent) {}

SpellChipButton::SpellChipButton(const QString& text, BadgeTone tone, QWidget* parent)
    : SpellChipButton(text, QString(), tone, parent) {}

SpellChipButton::SpellChipButton(const QString& text, const QString& iconName, QWidget* parent)
    : SpellChipButton(text, iconName, BadgeTone::Accent, parent) {}

SpellChipButton::SpellChipButton(const QString& text,
                                 const QString& iconName,
                                 BadgeTone tone,
                                 QWidget* parent)
    : QPushButton(text, parent)
    , m_tone(tone)
    , m_iconName(iconName) {
    initDefaults();
}

void SpellChipButton::initDefaults() {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setIconSize(QSize(kChipIconSize, kChipIconSize));

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SpellChipButton::applyTheme);
    connect(this, &QAbstractButton::toggled,
            this, [this](bool) { applyTheme(); });

    applyTheme();
}

void SpellChipButton::setTone(BadgeTone tone) {
    if (m_tone == tone) {
        return;
    }
    m_tone = tone;
    applyTheme();
}

void SpellChipButton::setIconName(const QString& iconName) {
    if (m_iconName == iconName) {
        return;
    }
    m_iconName = iconName;
    refreshIcon();
}

void SpellChipButton::setChecked(bool checked) {
    const bool wasChecked = isChecked();
    QPushButton::setChecked(checked);
    if (wasChecked == isChecked()) {
        applyTheme();
    }
}

void SpellChipButton::applyTheme() {
    setFont(makeUiFont(10.5, QFont::DemiBold));

    const UiPalette palette = ThemeManager::instance().ui();
    const QColor tone = badgeToneColor(m_tone, palette);

    const QColor offBg = palette.surface;
    const QColor offHoverBg = compositeOver(palette.elevatedSurface, palette.surface);
    const QColor offBorder = palette.subtleBorder;
    const QColor offText = palette.textSecondary;

    const QColor onBg = scaledAlpha(tone, 0.16);
    const QColor onHoverBg = scaledAlpha(tone, 0.22);
    const QColor onBorder = scaledAlpha(tone, 0.65);
    const QColor pressedBorder = scaledAlpha(tone, 0.75);

    setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background-color: %1;"
        "  color: %2;"
        "  border: 1px solid %3;"
        "  border-radius: 6px;"
        "  padding: 4px 9px;"
        "  font-size: 10.5pt;"
        "  font-weight: 600;"
        "}"
        "QPushButton:hover {"
        "  background-color: %4;"
        "  color: %2;"
        "  border-color: %3;"
        "}"
        "QPushButton:checked {"
        "  background-color: %5;"
        "  color: %6;"
        "  border: 1px solid %7;"
        "}"
        "QPushButton:checked:hover {"
        "  background-color: %8;"
        "  color: %6;"
        "  border: 1px solid %9;"
        "}"
        "QPushButton:pressed {"
        "  background-color: %8;"
        "  color: %6;"
        "  border: 1px solid %9;"
        "}"
        "QPushButton:disabled {"
        "  background-color: %1;"
        "  color: %10;"
        "  border-color: %11;"
        "}"
    ).arg(qssColor(offBg),
          qssColor(offText),
          qssColor(offBorder),
          qssColor(offHoverBg),
          qssColor(onBg),
          qssColor(tone),
          qssColor(onBorder),
          qssColor(onHoverBg),
          qssColor(pressedBorder),
          qssColor(palette.textMuted),
          qssColor(palette.divider)));

    refreshIcon();
    update();
}

void SpellChipButton::refreshIcon() {
    if (m_iconName.isEmpty()) {
        setIcon(QIcon());
        return;
    }
    const UiPalette palette = ThemeManager::instance().ui();
    const QColor iconColor = isChecked()
        ? badgeToneColor(m_tone, palette)
        : palette.textSecondary;
    setIconSize(QSize(kChipIconSize, kChipIconSize));
    setIcon(icons::vectorIcon(m_iconName, iconColor, kChipIconSize));
}

// ---------------------------------------------------------------------------
// SpellIconButton
// ---------------------------------------------------------------------------

SpellIconButton::SpellIconButton(QWidget* parent)
    : SpellIconButton(QString(), QString(), BadgeTone::Neutral, parent) {}

SpellIconButton::SpellIconButton(const QString& iconName, QWidget* parent)
    : SpellIconButton(iconName, QString(), BadgeTone::Neutral, parent) {}

SpellIconButton::SpellIconButton(const QString& iconName, BadgeTone tone, QWidget* parent)
    : SpellIconButton(iconName, QString(), tone, parent) {}

SpellIconButton::SpellIconButton(const QString& iconName, const QString& toolTip, QWidget* parent)
    : SpellIconButton(iconName, toolTip, BadgeTone::Neutral, parent) {}

SpellIconButton::SpellIconButton(const QString& iconName,
                                 const QString& toolTip,
                                 BadgeTone tone,
                                 QWidget* parent)
    : QToolButton(parent)
    , m_iconName(iconName)
    , m_tone(tone) {
    if (!toolTip.isEmpty()) {
        setToolTip(toolTip);
    }
    initDefaults();
}

void SpellIconButton::initDefaults() {
    setAutoRaise(true);
    setToolButtonStyle(Qt::ToolButtonIconOnly);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    setFixedSize(std::max(kIconBtnMinWidth, m_iconSize + 10),
                 std::max(kIconBtnMinHeight, m_iconSize + 8));

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SpellIconButton::applyTheme);
    connect(this, &QAbstractButton::toggled,
            this, [this](bool) { refreshIcon(); });

    applyTheme();
}

void SpellIconButton::setIconName(const QString& iconName) {
    if (m_iconName == iconName) {
        return;
    }
    m_iconName = iconName;
    refreshIcon();
}

void SpellIconButton::setTone(BadgeTone tone) {
    if (m_tone == tone) {
        return;
    }
    m_tone = tone;
    refreshIcon();
}

void SpellIconButton::setIconPixelSize(int px) {
    const int clamped = std::clamp(px, 8, 64);
    if (m_iconSize == clamped) {
        return;
    }
    m_iconSize = clamped;
    setFixedSize(std::max(kIconBtnMinWidth, m_iconSize + 10),
                 std::max(kIconBtnMinHeight, m_iconSize + 8));
    refreshIcon();
}

void SpellIconButton::applyTheme() {
    const UiPalette palette = ThemeManager::instance().ui();
    setStyleSheet(QStringLiteral(
        "QToolButton {"
        "  background-color: transparent;"
        "  border: 1px solid transparent;"
        "  border-radius: 6px;"
        "  padding: 0px;"
        "}"
        "QToolButton:hover {"
        "  background-color: %1;"
        "  border: 1px solid transparent;"
        "}"
        "QToolButton:pressed {"
        "  background-color: %2;"
        "  border: 1px solid transparent;"
        "}"
        "QToolButton:checked {"
        "  background-color: %2;"
        "  border: 1px solid %3;"
        "}"
    ).arg(qssColor(palette.elevatedSurface),
          qssColor(palette.selectedFill),
          qssColor(palette.selectedBorder)));

    refreshIcon();
    update();
}

void SpellIconButton::enterEvent(QEnterEvent* event) {
    m_hovered = true;
    refreshIcon();
    QToolButton::enterEvent(event);
}

void SpellIconButton::leaveEvent(QEvent* event) {
    m_hovered = false;
    refreshIcon();
    QToolButton::leaveEvent(event);
}

void SpellIconButton::refreshIcon() {
    setIconSize(QSize(m_iconSize, m_iconSize));
    if (m_iconName.isEmpty()) {
        setIcon(QIcon());
        return;
    }
    const UiPalette palette = ThemeManager::instance().ui();
    const bool brighten = (m_hovered && isEnabled()) || isChecked();
    const QColor glyphColor = brighten
        ? palette.textPrimary
        : badgeToneColor(m_tone, palette);
    setIcon(icons::vectorIcon(m_iconName, glyphColor, m_iconSize));
}

// ---------------------------------------------------------------------------
// SpellHairline
// ---------------------------------------------------------------------------

SpellHairline::SpellHairline(Qt::Orientation orientation, QWidget* parent)
    : QFrame(parent)
    , m_orientation(orientation) {
    setFrameShadow(QFrame::Plain);
    setLineWidth(0);
    setMidLineWidth(0);

    updateGeometryForOrientation();

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SpellHairline::applyTheme);

    applyTheme();
}

SpellHairline::SpellHairline(QWidget* parent)
    : SpellHairline(Qt::Horizontal, parent) {}

void SpellHairline::setOrientation(Qt::Orientation orientation) {
    if (m_orientation == orientation) {
        return;
    }
    m_orientation = orientation;
    updateGeometryForOrientation();
    applyTheme();
}

void SpellHairline::updateGeometryForOrientation() {
    if (m_orientation == Qt::Vertical) {
        setFrameShape(QFrame::VLine);
        setMinimumHeight(0);
        setMaximumHeight(QWIDGETSIZE_MAX);
        setFixedWidth(1);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    } else {
        setFrameShape(QFrame::HLine);
        setMinimumWidth(0);
        setMaximumWidth(QWIDGETSIZE_MAX);
        setFixedHeight(1);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    updateGeometry();
}

void SpellHairline::applyTheme() {
    const UiPalette palette = ThemeManager::instance().ui();
    const QColor dividerColor = compositeOver(palette.subtleBorder, palette.background);
    setStyleSheet(QStringLiteral(
        "QFrame { background-color: %1; color: %1; border: none; }"
    ).arg(qssColor(dividerColor)));

    QPalette pal = this->palette();
    pal.setColor(QPalette::Window, dividerColor);
    pal.setColor(QPalette::WindowText, dividerColor);
    setPalette(pal);

    update();
}

// ---------------------------------------------------------------------------
// SpellValueRow
// ---------------------------------------------------------------------------

SpellValueRow::SpellValueRow(QWidget* parent)
    : SpellValueRow(QString(), QString(), BadgeTone::Neutral, parent) {}

SpellValueRow::SpellValueRow(const QString& label, QWidget* parent)
    : SpellValueRow(label, QString(), BadgeTone::Neutral, parent) {}

SpellValueRow::SpellValueRow(const QString& label, const QString& value, QWidget* parent)
    : SpellValueRow(label, value, BadgeTone::Neutral, parent) {}

SpellValueRow::SpellValueRow(const QString& label,
                             const QString& value,
                             BadgeTone tone,
                             QWidget* parent)
    : QWidget(parent)
    , m_tone(tone)
    , m_valueIsPrimary(false) {
    setupUi();
    setLabel(label);
    setValue(value);
}

void SpellValueRow::setupUi() {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setStyleSheet(QStringLiteral("spell--SpellValueRow { background-color: transparent; }"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 1, 0, 1);
    layout->setSpacing(8);

    m_labelWidget = new QLabel(this);
    m_labelWidget->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_labelWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_labelWidget->setTextInteractionFlags(Qt::NoTextInteraction);

    m_valueWidget = new QLabel(this);
    m_valueWidget->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_valueWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_valueWidget->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_valueWidget->setCursor(Qt::IBeamCursor);

    layout->addWidget(m_labelWidget, 0, Qt::AlignVCenter);
    layout->addStretch(1);
    layout->addWidget(m_valueWidget, 0, Qt::AlignVCenter);

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SpellValueRow::applyTheme);

    applyTheme();
}

QString SpellValueRow::label() const {
    return m_labelWidget ? m_labelWidget->text() : QString();
}

QString SpellValueRow::value() const {
    return m_valueWidget ? m_valueWidget->text() : QString();
}

void SpellValueRow::setLabel(const QString& label) {
    if (m_labelWidget) {
        m_labelWidget->setText(label);
    }
}

void SpellValueRow::setValue(const QString& value) {
    if (m_valueWidget) {
        m_valueWidget->setText(value);
    }
}

void SpellValueRow::setTone(BadgeTone tone) {
    if (m_tone == tone) {
        return;
    }
    m_tone = tone;
    applyTheme();
}

void SpellValueRow::setValueIsPrimary(bool primary) {
    if (m_valueIsPrimary == primary) {
        return;
    }
    m_valueIsPrimary = primary;
    applyTheme();
}

void SpellValueRow::applyTheme() {
    const UiPalette palette = ThemeManager::instance().ui();
    const QColor labelColor = palette.textSecondary;
    const QColor valueColor = m_valueIsPrimary
        ? palette.textPrimary
        : badgeToneColor(m_tone, palette);

    if (m_labelWidget) {
        m_labelWidget->setFont(makeUiFont(11.0, QFont::Medium));
        m_labelWidget->setStyleSheet(QStringLiteral(
            "QLabel { background-color: transparent; color: %1; }"
        ).arg(qssColor(labelColor)));
        QPalette pal = m_labelWidget->palette();
        pal.setColor(QPalette::WindowText, labelColor);
        m_labelWidget->setPalette(pal);
    }

    if (m_valueWidget) {
        m_valueWidget->setFont(makeMonoFont(10.0, QFont::DemiBold));
        m_valueWidget->setStyleSheet(QStringLiteral(
            "QLabel { background-color: transparent; color: %1; }"
        ).arg(qssColor(valueColor)));
        QPalette pal = m_valueWidget->palette();
        pal.setColor(QPalette::WindowText, valueColor);
        m_valueWidget->setPalette(pal);
    }

    update();
}

// ---------------------------------------------------------------------------
// SpellSectionHeader
// ---------------------------------------------------------------------------

SpellSectionHeader::SpellSectionHeader(QWidget* parent)
    : SpellSectionHeader(QString(), QString(), BadgeTone::Accent, parent) {}

SpellSectionHeader::SpellSectionHeader(const QString& title, QWidget* parent)
    : SpellSectionHeader(title, QString(), BadgeTone::Accent, parent) {}

SpellSectionHeader::SpellSectionHeader(const QString& title, BadgeTone tone, QWidget* parent)
    : SpellSectionHeader(title, QString(), tone, parent) {}

SpellSectionHeader::SpellSectionHeader(const QString& title,
                                       const QString& iconName,
                                       QWidget* parent)
    : SpellSectionHeader(title, iconName, BadgeTone::Accent, parent) {}

SpellSectionHeader::SpellSectionHeader(const QString& title,
                                       const QString& iconName,
                                       BadgeTone tone,
                                       QWidget* parent)
    : QWidget(parent)
    , m_title(title)
    , m_iconName(iconName)
    , m_tone(tone) {
    setupUi();
}

void SpellSectionHeader::setupUi() {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setStyleSheet(QStringLiteral("spell--SpellSectionHeader { background-color: transparent; }"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);

    m_iconLabel = new QLabel(this);
    m_iconLabel->setFixedSize(kSectionIconSize, kSectionIconSize);
    m_iconLabel->setAlignment(Qt::AlignCenter);
    m_iconLabel->setStyleSheet(QStringLiteral("QLabel { background-color: transparent; border: none; }"));

    m_titleLabel = new QLabel(this);
    m_titleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_titleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    layout->addWidget(m_iconLabel, 0, Qt::AlignVCenter);
    layout->addWidget(m_titleLabel, 1, Qt::AlignVCenter);

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SpellSectionHeader::applyTheme);

    applyTheme();
}

void SpellSectionHeader::setTitle(const QString& title) {
    if (m_title == title) {
        return;
    }
    m_title = title;
    refreshVisuals();
}

void SpellSectionHeader::setIconName(const QString& iconName) {
    if (m_iconName == iconName) {
        return;
    }
    m_iconName = iconName;
    refreshVisuals();
}

void SpellSectionHeader::setTone(BadgeTone tone) {
    if (m_tone == tone) {
        return;
    }
    m_tone = tone;
    refreshVisuals();
}

void SpellSectionHeader::applyTheme() {
    refreshVisuals();
}

void SpellSectionHeader::refreshVisuals() {
    const UiPalette palette = ThemeManager::instance().ui();
    const QColor toneColor = badgeToneColor(m_tone, palette);

    if (m_iconLabel) {
        if (m_iconName.isEmpty()) {
            m_iconLabel->clear();
            m_iconLabel->setVisible(false);
        } else {
            const QIcon icon = icons::vectorIcon(m_iconName, toneColor, kSectionIconSize);
            const qreal dpr = std::max<qreal>(1.0, devicePixelRatioF());
            m_iconLabel->setPixmap(icon.pixmap(QSize(kSectionIconSize, kSectionIconSize), dpr));
            m_iconLabel->setVisible(true);
        }
    }

    if (m_titleLabel) {
        QFont monoFont = makeMonoFont(10.0, QFont::Bold);
        monoFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
        m_titleLabel->setFont(monoFont);
        m_titleLabel->setText(m_title.toUpper());
        m_titleLabel->setStyleSheet(QStringLiteral(
            "QLabel { background-color: transparent; color: %1; }"
        ).arg(qssColor(toneColor)));

        QPalette pal = m_titleLabel->palette();
        pal.setColor(QPalette::WindowText, toneColor);
        m_titleLabel->setPalette(pal);
    }

    update();
}

} // namespace spell
