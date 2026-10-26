// SPDX-License-Identifier: MIT
//
// Closing on Linux: delete-event, which GTK 3 sends when the window manager's
// close button is pressed and when gtk_window_close() is called, and GTK 4's
// close-request in its place. Answering TRUE keeps the window; the webview's
// own handler is on "destroy", which only comes once it goes.
#include "closing.hpp"

#include <utility>

#include <gtk/gtk.h>

namespace crucible::gui::closing {
namespace {

struct Watch {
    std::function<bool()> may_close;
};

gboolean held(Watch* watch) { return watch->may_close() ? FALSE : TRUE; }

}  // namespace

bool watch(void* window, std::function<bool()> may_close) {
    if (window == nullptr || !GTK_IS_WINDOW(window)) {
        return false;
    }
    // Freed with the handler, when the window is.
    auto* kept = new Watch{std::move(may_close)};
    const GClosureNotify forget = [](gpointer data, GClosure*) { delete static_cast<Watch*>(data); };
#if GTK_MAJOR_VERSION >= 4
    g_signal_connect_data(window, "close-request",
                          G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
                              return held(static_cast<Watch*>(data));
                          }),
                          kept, forget, GConnectFlags(0));
#else
    g_signal_connect_data(window, "delete-event",
                          G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
                              return held(static_cast<Watch*>(data));
                          }),
                          kept, forget, GConnectFlags(0));
#endif
    return true;
}

}  // namespace crucible::gui::closing
