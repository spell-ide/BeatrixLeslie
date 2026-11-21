#pragma once

#include <QColor>

class QWidget;

namespace spell {

/**
 * Configures a top-level window using cross-platform Qt 6 native window flags
 * (`Qt::ExpandedClientAreaHint` and `Qt::NoTitleBarBackgroundHint` on Qt 6.9+)
 * so content fills all the way to the top edge (`y = 0`) with no separate title
 * bar strip, and keeps the native window palette background synced with the
 * active theme across macOS, Linux, and Windows.
 */
void installFramelessTitlebarChrome(QWidget* window);

/** Updates the window's auto-fill background and `QPalette::Window` color (e.g. on theme change). */
void applyFramelessTitlebarChrome(QWidget* window, const QColor& backgroundColor);

/**
 * Configures a sheet/settings dialog with `Qt::FramelessWindowHint` while
 * preserving `Qt::Dialog` modality, Escape-to-reject, and keyboard focus across
 * macOS, Linux, and Windows. Pair with `installWindowDragRegion` on the
 * dialog's top bar and/or sidebar so empty areas remain draggable.
 */
void installFramelessSheetChrome(QWidget* dialog);

/**
 * Makes empty regions, non-interactive `QLabel`s, and transparent spacers of a
 * surface widget act as a cross-platform window drag handle and double-click
 * maximize/restore target across macOS, Linux (X11 & Wayland), and Windows,
 * while ignoring buttons, tabs, and input controls.
 */
void installWindowDragRegion(QWidget* dragSurface);

/**
 * Pop-out terminal windows only: restricts native title-bar buttons to Close + Minimize
 * (no zoom/maximize/fullscreen) and installs OS-portable window shortcuts:
 *   macOS:          Cmd+W close, Cmd+M minimize
 *   Linux/Windows:  Ctrl+Shift+W close, Ctrl+Shift+M minimize
 *                   (plain Ctrl+W / Ctrl+M stay with the shell: delete-word / Enter)
 * Call before the window is first shown.
 */
void installTerminalWindowControls(QWidget* window);

/** True if `event` is one of the terminal window-control shortcuts above. */
bool isTerminalWindowControlShortcut(int key, Qt::KeyboardModifiers mods);

} // namespace spell
