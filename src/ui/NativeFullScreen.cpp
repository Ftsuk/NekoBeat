#include "ui/NativeFullScreen.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

bool NativeFullScreen::isSupported()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

void NativeFullScreen::useBlackBackground(WId window)
{
#ifdef Q_OS_WIN
    if (!window)
        return;
    const HWND hwnd = reinterpret_cast<HWND>(window);
    // The stock brush is owned by the system and stays valid for the lifetime of
    // the process, so installing it repeatedly is harmless.
    SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND,
                     reinterpret_cast<LONG_PTR>(GetStockObject(BLACK_BRUSH)));
#else
    Q_UNUSED(window);
#endif
}

bool NativeFullScreen::captureWindowState(WId window, QRect* frameRect, qintptr* style)
{
#ifdef Q_OS_WIN
    if (!window)
        return false;

    const HWND hwnd = reinterpret_cast<HWND>(window);
    RECT frame{};
    if (!GetWindowRect(hwnd, &frame))
        return false;

    if (frameRect)
    {
        *frameRect = QRect(frame.left, frame.top,
                           frame.right - frame.left, frame.bottom - frame.top);
    }
    if (style)
        *style = static_cast<qintptr>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    return true;
#else
    Q_UNUSED(window);
    Q_UNUSED(frameRect);
    Q_UNUSED(style);
    return false;
#endif
}

void NativeFullScreen::enter(WId window)
{
#ifdef Q_OS_WIN
    if (!window)
        return;

    // A style change, not a window-state change: setWindowFlags() would destroy
    // and recreate the native window (taking the GL and mpv render contexts with
    // it), and showFullScreen() makes Windows walk the "restore to a small
    // window, then maximize" path - which is what the user sees as the window
    // shrinking for a moment and what blocks the event loop for hundreds of
    // milliseconds while mpv re-configures its video chain.
    const HWND hwnd = reinterpret_cast<HWND>(window);
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE,
                      (style & ~(WS_CAPTION | WS_THICKFRAME)) | WS_POPUP);

    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfoW(monitor, &info))
        return;

    // Monitor rectangles are physical pixels, exactly what SetWindowPos expects,
    // so per-monitor DPI needs no conversion here.
    //
    // The window is deliberately grown by one pixel on every side instead of
    // matching the monitor rectangle exactly. A borderless window whose client
    // area covers the monitor *exactly* qualifies for Windows' full screen
    // optimisation (DWM switches to direct/independent flip and scans this
    // window out on its own). In that mode no other top level window is
    // composited any more: Qt's popup menus, their - very real - windows report
    // "visible, topmost, correct rectangle" to Win32 while never appearing on
    // screen, and the menu's modal loop keeps grabbing the mouse, which reads
    // as "the dropdown opened but is invisible and the UI is stuck until I
    // click the picture". One pixel of overflow keeps the window out of that
    // optimisation while being invisible to the eye.
    SetWindowPos(hwnd, HWND_TOP,
                 info.rcMonitor.left - 1, info.rcMonitor.top - 1,
                 info.rcMonitor.right - info.rcMonitor.left + 2,
                 info.rcMonitor.bottom - info.rcMonitor.top + 2,
                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
#else
    Q_UNUSED(window);
#endif
}

void NativeFullScreen::leave(WId window, qintptr style, bool wasMaximized,
                             const QRect& restoreRect)
{
#ifdef Q_OS_WIN
    if (!window)
        return;

    const HWND hwnd = reinterpret_cast<HWND>(window);
    SetWindowLongPtrW(hwnd, GWL_STYLE, static_cast<LONG_PTR>(style));

    if (wasMaximized)
    {
        // Windows computes the maximized rectangle itself, so only the frame has
        // to be recomputed; this is the cheap half of the old two-step dance.
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        ShowWindow(hwnd, SW_MAXIMIZE);
        return;
    }

    SetWindowPos(hwnd, nullptr, restoreRect.left(), restoreRect.top(),
                 restoreRect.width(), restoreRect.height(),
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
#else
    Q_UNUSED(window);
    Q_UNUSED(style);
    Q_UNUSED(wasMaximized);
    Q_UNUSED(restoreRect);
#endif
}

bool NativeFullScreen::isMinimizeNotification(const void* message)
{
#ifdef Q_OS_WIN
    if (!message)
        return false;
    const MSG* msg = static_cast<const MSG*>(message);
    return msg->message == WM_SIZE && msg->wParam == SIZE_MINIMIZED;
#else
    Q_UNUSED(message);
    return false;
#endif
}
