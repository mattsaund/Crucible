// SPDX-License-Identifier: MIT
//
// Closing on Windows: WM_CLOSE, which the title bar's button, Alt+F4 and the
// taskbar's Close window all send. The webview's window procedure destroys
// the window on it; this one is put in front of that and passes the message
// on only when the close may go ahead. The webview takes its own procedure
// back before it destroys the window, so this is never asked on the way out.
#include "closing.hpp"

#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace crucible::gui::closing {
namespace {

// One window per process, so one of each.
WNDPROC               g_previous = nullptr;
std::function<bool()> g_may_close;

LRESULT CALLBACK closing_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CLOSE && g_may_close && !g_may_close()) {
        return 0;   // kept open while the page asks
    }
    return CallWindowProcW(g_previous, window, message, wparam, lparam);
}

}  // namespace

bool watch(void* window, std::function<bool()> may_close) {
    const HWND handle = static_cast<HWND>(window);
    if (handle == nullptr || !IsWindow(handle) || g_previous != nullptr) {
        return false;
    }
    g_may_close = std::move(may_close);
    g_previous  = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(handle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&closing_procedure)));
    return g_previous != nullptr;
}

}  // namespace crucible::gui::closing
