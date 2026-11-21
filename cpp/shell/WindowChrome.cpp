#include "shell/WindowChrome.h"

#include "theme/SpellTheme.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QEvent>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QKeySequence>
#include <QShortcut>
#include <QMenuBar>
#include <QMouseEvent>
#include <QObject>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPoint>
#include <QPointer>
#include <QSplitterHandle>
#include <QTabBar>
#include <QTextEdit>
#include <QMainWindow>
#include <QWidget>
#include <QWindow>
#include <QtGlobal>

#include <typeinfo>

namespace spell {

namespace {

bool isWaylandPlatform() {
    return QGuiApplication::platformName().contains(
        QLatin1StringView("wayland"), Qt::CaseInsensitive);
}

void applyWindowBackground(QWidget* window, const QColor& backgroundColor) {
    if (!window) {
        return;
    }
    window->setAutoFillBackground(true);
    if (!backgroundColor.isValid()) {
        return;
    }
    QPalette pal = window->palette();
    if (pal.color(QPalette::Active, QPalette::Window) != backgroundColor ||
        pal.color(QPalette::Inactive, QPalette::Window) != backgroundColor) {
        pal.setColor(QPalette::Window, backgroundColor);
        window->setPalette(pal);
    }
}

void ensureExpandedClientAreaFlags(QWidget* window) {
    if (!window) {
        return;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    const Qt::WindowFlags current = window->windowFlags();
    const Qt::WindowFlags required = Qt::ExpandedClientAreaHint | Qt::NoTitleBarBackgroundHint;
    if ((current & required) != required) {
        const bool wasVisible = window->isVisible();
        window->setWindowFlag(Qt::ExpandedClientAreaHint, true);
        window->setWindowFlag(Qt::NoTitleBarBackgroundHint, true);
        if (wasVisible) {
            window->show();
        }
    }
#else
    Q_UNUSED(window);
#endif
}

bool isInteractiveOrCustomWidget(const QWidget* widget) {
    if (!widget) {
        return false;
    }
    if (widget->testAttribute(Qt::WA_TransparentForMouseEvents)) {
        return false;
    }

    // Never drag when clicking standard interactive controls, inputs, tabs, or handles.
    if (qobject_cast<const QAbstractButton*>(widget) != nullptr ||
        qobject_cast<const QLineEdit*>(widget) != nullptr ||
        qobject_cast<const QTextEdit*>(widget) != nullptr ||
        qobject_cast<const QPlainTextEdit*>(widget) != nullptr ||
        qobject_cast<const QAbstractSpinBox*>(widget) != nullptr ||
        qobject_cast<const QComboBox*>(widget) != nullptr ||
        qobject_cast<const QAbstractSlider*>(widget) != nullptr ||
        qobject_cast<const QAbstractItemView*>(widget) != nullptr ||
        qobject_cast<const QTabBar*>(widget) != nullptr ||
        qobject_cast<const QMenuBar*>(widget) != nullptr ||
        qobject_cast<const QMenu*>(widget) != nullptr ||
        qobject_cast<const QSplitterHandle*>(widget) != nullptr) {
        return true;
    }

    // Non-interactive QLabels (e.g. "Projects", "SETTINGS", header titles/icons) act as
    // draggable surface background; selectable or link labels remain interactive.
    if (const auto* label = qobject_cast<const QLabel*>(widget)) {
        return (label->textInteractionFlags() != Qt::NoTextInteraction) ||
               label->openExternalLinks() ||
               (label->testAttribute(Qt::WA_SetCursor) &&
                label->cursor().shape() != Qt::ArrowCursor);
    }

    // Plain non-interactive QWidget layout containers / spacers are transparent drag
    // background; custom QWidget subclasses (such as FileTabWidget, ConversationTabWidget,
    // InteractiveRowWidget) have a distinct dynamic type and are not draggable.
    const QWidget& widgetRef = *widget;
    if (typeid(widgetRef) == typeid(QWidget)) {
        return (widget->focusPolicy() != Qt::NoFocus) ||
               (widget->testAttribute(Qt::WA_SetCursor) &&
                widget->cursor().shape() != Qt::ArrowCursor);
    }

    return true;
}

class WindowDragFilter final : public QObject {
public:
    explicit WindowDragFilter(QWidget* surface)
        : QObject(surface)
        , m_surface(surface) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched != m_surface || !event || !m_surface) {
            return QObject::eventFilter(watched, event);
        }

        switch (event->type()) {
        case QEvent::MouseButtonDblClick: {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton && isEmptyHit(me->position().toPoint())) {
                m_dragging = false;
                if (QWidget* win = m_surface->window()) {
                    if (win->isMaximized()) {
                        win->showNormal();
                    } else if (!win->isFullScreen()) {
                        win->showMaximized();
                    }
                    me->accept();
                    return true;
                }
            }
            break;
        }
        case QEvent::MouseButtonPress: {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton && isEmptyHit(me->position().toPoint())) {
                if (QWidget* win = m_surface->window()) {
                    if (win->isFullScreen()) {
                        break;
                    }
                    // Prefer the compositor/window-manager move (macOS, Windows, X11, Wayland);
                    // fall back to manual move() tracking if unsupported.
                    if (QWindow* handle = win->windowHandle()) {
                        if (handle->startSystemMove()) {
                            m_dragging = false;
                            me->accept();
                            return true;
                        }
                    }
                    {
                        m_dragging = true;
                        m_dragOffset =
                            me->globalPosition().toPoint() - win->frameGeometry().topLeft();
                        me->accept();
                        return true;
                    }
                }
            }
            break;
        }
        case QEvent::MouseMove: {
            if (m_dragging) {
                auto* me = static_cast<QMouseEvent*>(event);
                if (me->buttons() & Qt::LeftButton) {
                    if (QWidget* win = m_surface->window()) {
                        const QPoint targetTopLeft =
                            me->globalPosition().toPoint() - m_dragOffset;
                        if (targetTopLeft != win->frameGeometry().topLeft()) {
                            win->move(targetTopLeft);
                        }
                        me->accept();
                        return true;
                    }
                } else {
                    m_dragging = false;
                }
            }
            break;
        }
        case QEvent::MouseButtonRelease: {
            if (m_dragging) {
                auto* me = static_cast<QMouseEvent*>(event);
                if (me->button() == Qt::LeftButton || !(me->buttons() & Qt::LeftButton)) {
                    m_dragging = false;
                    me->accept();
                    return true;
                }
            }
            break;
        }
        case QEvent::Hide:
        case QEvent::WindowDeactivate:
        case QEvent::UngrabMouse:
            m_dragging = false;
            break;
        default:
            break;
        }

        return QObject::eventFilter(watched, event);
    }

private:
    bool isEmptyHit(const QPoint& localPos) const {
        if (!m_surface) {
            return false;
        }
        QWidget* child = m_surface->childAt(localPos);
        if (!child || child == m_surface) {
            return true;
        }
        for (const QWidget* w = child; w && w != m_surface; w = w->parentWidget()) {
            if (isInteractiveOrCustomWidget(w)) {
                return false;
            }
        }
        return true;
    }

    QPointer<QWidget> m_surface;
    bool m_dragging = false;
    QPoint m_dragOffset;
};

} // namespace

void applyFramelessTitlebarChrome(QWidget* window, const QColor& backgroundColor) {
    applyWindowBackground(window, backgroundColor);
}

void installFramelessTitlebarChrome(QWidget* window) {
    if (!window) {
        return;
    }

    ensureExpandedClientAreaFlags(window);
    // Let content (34px top bars) extend under the titlebar instead of being pushed
    // down by the safe-area margins Qt applies by default.
    window->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    if (auto* mw = qobject_cast<QMainWindow*>(window)) {
        if (QWidget* cw = mw->centralWidget()) {
            cw->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
            for (QWidget* child : cw->findChildren<QWidget*>()) {
                child->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
            }
        }
    }

    if (!window->property("_spellFramelessChromeInstalled").toBool()) {
        window->setProperty("_spellFramelessChromeInstalled", true);

        QPointer<QWidget> ptr(window);
        QObject::connect(&ThemeManager::instance(), &ThemeManager::themeChanged, window, [ptr]() {
            if (ptr) {
                applyFramelessTitlebarChrome(ptr.data(), ThemeManager::instance().ui().background);
            }
        });
    }

    applyFramelessTitlebarChrome(window, ThemeManager::instance().ui().background);
}

void installFramelessSheetChrome(QWidget* dialog) {
    if (!dialog) {
        return;
    }

    Qt::WindowFlags flags = dialog->windowFlags();
    if ((flags & Qt::WindowType_Mask) != Qt::Dialog) {
        flags = (flags & ~Qt::WindowType_Mask) | Qt::Dialog;
    }
    flags |= Qt::FramelessWindowHint;

    if (flags != dialog->windowFlags()) {
        const bool wasVisible = dialog->isVisible();
        dialog->setWindowFlags(flags);
        if (wasVisible) {
            dialog->show();
        }
    }

    if (!dialog->property("_spellFramelessSheetInstalled").toBool()) {
        dialog->setProperty("_spellFramelessSheetInstalled", true);

        QPointer<QWidget> ptr(dialog);
        QObject::connect(&ThemeManager::instance(), &ThemeManager::themeChanged, dialog, [ptr]() {
            if (ptr) {
                applyWindowBackground(ptr.data(), ThemeManager::instance().ui().background);
            }
        });
    }

    applyWindowBackground(dialog, ThemeManager::instance().ui().background);
}

void installWindowDragRegion(QWidget* dragSurface) {
    if (!dragSurface) {
        return;
    }
    if (dragSurface->property("_spellDragRegionInstalled").toBool()) {
        return;
    }
    dragSurface->setProperty("_spellDragRegionInstalled", true);
    auto* filter = new WindowDragFilter(dragSurface);
    dragSurface->installEventFilter(filter);
}

bool isTerminalWindowControlShortcut(int key, Qt::KeyboardModifiers mods) {
    mods &= ~Qt::KeypadModifier;
    if (key != Qt::Key_W && key != Qt::Key_M) {
        return false;
    }
#ifdef Q_OS_MACOS
    // Qt maps the Command key to ControlModifier on macOS.
    return mods == Qt::ControlModifier;
#else
    return mods == (Qt::ControlModifier | Qt::ShiftModifier);
#endif
}

void installTerminalWindowControls(QWidget* window) {
    if (!window || window->property("_spellTerminalControlsInstalled").toBool()) {
        return;
    }
    window->setProperty("_spellTerminalControlsInstalled", true);

    // Native buttons: Close + Minimize only.
    Qt::WindowFlags flags = window->windowFlags();
    flags |= Qt::Window | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
             Qt::WindowCloseButtonHint | Qt::WindowMinimizeButtonHint;
    flags &= ~(Qt::WindowMaximizeButtonHint | Qt::WindowFullscreenButtonHint);
    window->setWindowFlags(flags);

#ifdef Q_OS_MACOS
    const QKeySequence closeSeq(Qt::CTRL | Qt::Key_W);
    const QKeySequence minSeq(Qt::CTRL | Qt::Key_M);
#else
    const QKeySequence closeSeq(Qt::CTRL | Qt::SHIFT | Qt::Key_W);
    const QKeySequence minSeq(Qt::CTRL | Qt::SHIFT | Qt::Key_M);
#endif

    auto* closeShortcut = new QShortcut(closeSeq, window);
    closeShortcut->setObjectName(QStringLiteral("spellTerminalCloseShortcut"));
    closeShortcut->setContext(Qt::WindowShortcut);
    QObject::connect(closeShortcut, &QShortcut::activated, window, [window]() { window->close(); });

    auto* minShortcut = new QShortcut(minSeq, window);
    minShortcut->setObjectName(QStringLiteral("spellTerminalMinimizeShortcut"));
    minShortcut->setContext(Qt::WindowShortcut);
    QObject::connect(minShortcut, &QShortcut::activated, window, [window]() { window->showMinimized(); });
}

} // namespace spell
