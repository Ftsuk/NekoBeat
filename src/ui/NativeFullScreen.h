#pragma once

#include <QRect>
#include <QtGlobal>
#include <QtGui/qwindowdefs.h>

// Win32 native full screen helpers. They live in their own translation unit so
// <windows.h> (and its DeviceCapabilities / min / max macros) never leaks into
// the window code. On other platforms they report "unsupported" and the caller
// falls back to Qt's showFullScreen().
namespace NativeFullScreen
{
bool isSupported();

// Windows erases newly exposed window areas with the window class background
// brush, which is white by default - that is the white flash visible along the
// edges while the window grows into full screen. Setting it to a black brush
// makes those erase steps invisible against the dark UI.
void useBlackBackground(WId window);

// Frame rectangle in physical pixels plus the window style bits. Returns false
// when the platform has no native route.
bool captureWindowState(WId window, QRect* frameRect, qintptr* style);

// Strips the caption and frame and covers the window's monitor.
void enter(WId window);

// Puts the saved style back and restores the previous geometry.
void leave(WId window, qintptr style, bool wasMaximized, const QRect& restoreRect);

// True for the "window was minimized" notification: native full screen has to
// be left before Windows restores a window with a stripped frame.
bool isMinimizeNotification(const void* message);
}
