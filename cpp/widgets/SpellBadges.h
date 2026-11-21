#pragma once

#include "theme/SpellTheme.h"

#include <QColor>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QSize>
#include <QString>
#include <QToolButton>
#include <QWidget>

class QEnterEvent;
class QEvent;
class QPaintEvent;

namespace spell {

/** Semantic visual tone resolved against the active monochrome `UiPalette`. */
enum class BadgeTone {
    Accent,
    Success,
    Warning,
    Danger,
    Info,
    Neutral
};

/** Resolves a `BadgeTone` to its semantic color in `palette`. */
QColor badgeToneColor(BadgeTone tone, const UiPalette& palette);

/** Convenience overload resolving `tone` against `ThemeManager::instance().ui()`. */
QColor badgeToneColor(BadgeTone tone);

/**
 * Monochrome/tone capsule badge with an optional 6px status indicator dot.
 *
 * Paints an anti-aliased pill background and hairline border in `paintEvent` so
 * `QLabel::text()` always returns the clean unmodified badge text.
 */
class SpellBadge : public QLabel {
    Q_OBJECT

public:
    explicit SpellBadge(QWidget* parent = nullptr);
    explicit SpellBadge(const QString& text, QWidget* parent = nullptr);
    SpellBadge(const QString& text, BadgeTone tone, QWidget* parent = nullptr);
    SpellBadge(const QString& text, BadgeTone tone, bool showDot, QWidget* parent = nullptr);
    ~SpellBadge() override = default;

    BadgeTone tone() const { return m_tone; }
    bool showDot() const { return m_showDot; }
    bool isFilled() const { return m_filled; }

    void setText(const QString& text);
    void setTone(BadgeTone tone);
    void setShowDot(bool showDot);
    void setFilled(bool filled);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

public slots:
    void applyTheme();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void initDefaults();
    void updateMetrics();

    BadgeTone m_tone = BadgeTone::Neutral;
    bool m_showDot = false;
    bool m_filled = true;
};

/**
 * Rounded toggleable chip button used across pane toolbars and filter bars.
 *
 * Checkable by default (`setCheckable(true)`). Automatically re-tints its vector
 * icon (from `spell::icons::vectorIcon`) and border/fill styling on theme or check
 * state changes.
 */
class SpellChipButton : public QPushButton {
    Q_OBJECT

public:
    explicit SpellChipButton(QWidget* parent = nullptr);
    explicit SpellChipButton(const QString& text, QWidget* parent = nullptr);
    SpellChipButton(const QString& text, BadgeTone tone, QWidget* parent = nullptr);
    SpellChipButton(const QString& text, const QString& iconName, QWidget* parent = nullptr);
    SpellChipButton(const QString& text,
                    const QString& iconName,
                    BadgeTone tone,
                    QWidget* parent = nullptr);
    ~SpellChipButton() override = default;

    BadgeTone tone() const { return m_tone; }
    QString iconName() const { return m_iconName; }

    void setTone(BadgeTone tone);
    void setIconName(const QString& iconName);

public slots:
    void setChecked(bool checked);
    void applyTheme();

private:
    void initDefaults();
    void refreshIcon();

    BadgeTone m_tone = BadgeTone::Accent;
    QString m_iconName;
};

/**
 * Compact 24x22 hover icon button backed by `spell::icons::vectorIcon`.
 *
 * Highlights background subtly on hover and brightens the vector glyph from its
 * semantic tone color to `UiPalette::textPrimary`.
 */
class SpellIconButton : public QToolButton {
    Q_OBJECT

public:
    explicit SpellIconButton(QWidget* parent = nullptr);
    explicit SpellIconButton(const QString& iconName, QWidget* parent = nullptr);
    SpellIconButton(const QString& iconName, BadgeTone tone, QWidget* parent = nullptr);
    SpellIconButton(const QString& iconName, const QString& toolTip, QWidget* parent = nullptr);
    SpellIconButton(const QString& iconName,
                    const QString& toolTip,
                    BadgeTone tone,
                    QWidget* parent = nullptr);
    ~SpellIconButton() override = default;

    QString iconName() const { return m_iconName; }
    BadgeTone tone() const { return m_tone; }
    int iconPixelSize() const { return m_iconSize; }

    void setIconName(const QString& iconName);
    void setTone(BadgeTone tone);
    void setIconPixelSize(int px);

public slots:
    void applyTheme();

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void initDefaults();
    void refreshIcon();

    QString m_iconName;
    BadgeTone m_tone = BadgeTone::Neutral;
    int m_iconSize = 14;
    bool m_hovered = false;
};

/** 1px horizontal or vertical theme-aware hairline divider. */
class SpellHairline : public QFrame {
    Q_OBJECT

public:
    explicit SpellHairline(Qt::Orientation orientation = Qt::Horizontal,
                           QWidget* parent = nullptr);
    explicit SpellHairline(QWidget* parent);
    ~SpellHairline() override = default;

    Qt::Orientation orientation() const { return m_orientation; }
    void setOrientation(Qt::Orientation orientation);

public slots:
    void applyTheme();

private:
    void updateGeometryForOrientation();

    Qt::Orientation m_orientation = Qt::Horizontal;
};

/**
 * Two-column inspector/settings row with a left-aligned caption label and a
 * right-aligned monospaced, mouse-selectable value label.
 */
class SpellValueRow : public QWidget {
    Q_OBJECT

public:
    explicit SpellValueRow(QWidget* parent = nullptr);
    explicit SpellValueRow(const QString& label, QWidget* parent = nullptr);
    SpellValueRow(const QString& label, const QString& value, QWidget* parent = nullptr);
    SpellValueRow(const QString& label,
                  const QString& value,
                  BadgeTone tone,
                  QWidget* parent = nullptr);
    ~SpellValueRow() override = default;

    QString label() const;
    QString value() const;
    BadgeTone tone() const { return m_tone; }
    bool valueIsPrimary() const { return m_valueIsPrimary; }

    void setLabel(const QString& label);
    void setValue(const QString& value);
    void setTone(BadgeTone tone);
    void setValueIsPrimary(bool primary);

    QLabel* labelWidget() const { return m_labelWidget; }
    QLabel* valueWidget() const { return m_valueWidget; }

public slots:
    void applyTheme();

private:
    void setupUi();

    QLabel* m_labelWidget = nullptr;
    QLabel* m_valueWidget = nullptr;
    BadgeTone m_tone = BadgeTone::Neutral;
    bool m_valueIsPrimary = false;
};

/**
 * Compact uppercase monospace section header with an optional leading vector icon.
 */
class SpellSectionHeader : public QWidget {
    Q_OBJECT

public:
    explicit SpellSectionHeader(QWidget* parent = nullptr);
    explicit SpellSectionHeader(const QString& title, QWidget* parent = nullptr);
    SpellSectionHeader(const QString& title, BadgeTone tone, QWidget* parent = nullptr);
    SpellSectionHeader(const QString& title, const QString& iconName, QWidget* parent = nullptr);
    SpellSectionHeader(const QString& title,
                       const QString& iconName,
                       BadgeTone tone,
                       QWidget* parent = nullptr);
    ~SpellSectionHeader() override = default;

    QString title() const { return m_title; }
    QString iconName() const { return m_iconName; }
    BadgeTone tone() const { return m_tone; }

    void setTitle(const QString& title);
    void setIconName(const QString& iconName);
    void setTone(BadgeTone tone);

public slots:
    void applyTheme();

private:
    void setupUi();
    void refreshVisuals();

    QLabel* m_iconLabel = nullptr;
    QLabel* m_titleLabel = nullptr;
    QString m_title;
    QString m_iconName;
    BadgeTone m_tone = BadgeTone::Accent;
};

} // namespace spell
