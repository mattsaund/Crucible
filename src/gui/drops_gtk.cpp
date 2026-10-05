// SPDX-License-Identifier: MIT
//
// Dropped files on Linux: the paths, read off the drop as GTK delivers it.
//
// WebKit is a drop target already, and it is left to be one -- the page still
// sees the drop, which is how it knows to draw the overlay and when to stop.
// These handlers run before WebKit's (the signals run their class handler
// last) and only listen: they return FALSE so WebKit carries on as it would.
//
// The paths come in "drag-data-received", which WebKit asks for while the
// pointer is still moving, so they are kept until "drag-drop" says this drag
// is the one that landed. They are handed over from an idle callback rather
// than from inside the signal, so the page hears of them after WebKit has
// finished with the drop rather than in the middle of it.
#include "drops.hpp"

#include <memory>
#include <utility>

#include <gtk/gtk.h>

namespace crucible::gui::drops {

#if GTK_MAJOR_VERSION >= 4

bool watch(void* /*widget*/, std::function<void(std::vector<std::string>)> /*dropped*/) {
    // GTK 4's drop target is a controller that WebKit owns; a second one on
    // the same widget would take the drop from it. The page reads it instead.
    return false;
}

#else

namespace {

struct Watch {
    std::function<void(std::vector<std::string>)> dropped;
    std::vector<std::string> paths;     ///< from the drag last heard of
    GdkDragContext*          from    = nullptr;
    bool                     landing = false;   ///< dropped, paths not yet heard
};

/// Hand `paths` over once the main loop is idle.
void deliver(Watch* watch, std::vector<std::string> paths) {
    struct Delivery {
        Watch*                   watch;
        std::vector<std::string> paths;
    };
    g_idle_add(
        [](gpointer data) -> gboolean {
            const std::unique_ptr<Delivery> delivery(static_cast<Delivery*>(data));
            delivery->watch->dropped(std::move(delivery->paths));
            return G_SOURCE_REMOVE;
        },
        new Delivery{watch, std::move(paths)});
}

void on_received(GtkWidget*, GdkDragContext* context, gint, gint, GtkSelectionData* data, guint,
                 guint, gpointer user) {
    auto* watch = static_cast<Watch*>(user);
    gchar** uris = gtk_selection_data_get_uris(data);
    if (uris == nullptr) {
        return;   // text, or a picture's bytes: not files, and not this
    }
    std::vector<std::string> paths;
    for (gchar** uri = uris; *uri != nullptr; ++uri) {
        // A link dragged from a browser is a URI too, and not a file.
        if (gchar* path = g_filename_from_uri(*uri, nullptr, nullptr)) {
            paths.emplace_back(path);
            g_free(path);
        }
    }
    g_strfreev(uris);
    if (watch->landing) {
        watch->landing = false;
        if (!paths.empty()) {
            deliver(watch, std::move(paths));
        }
        return;
    }
    watch->paths = std::move(paths);
    watch->from  = context;
}

gboolean on_drop(GtkWidget*, GdkDragContext* context, gint, gint, guint, gpointer user) {
    auto* watch = static_cast<Watch*>(user);
    if (watch->from == context && !watch->paths.empty()) {
        deliver(watch, std::exchange(watch->paths, {}));
    } else {
        // Not heard yet: whatever WebKit asks for now is this drop's.
        watch->landing = true;
    }
    watch->from = nullptr;
    return FALSE;
}

}  // namespace

bool watch(void* widget, std::function<void(std::vector<std::string>)> dropped) {
    if (widget == nullptr || !GTK_IS_WIDGET(widget)) {
        return false;
    }
    // Lives as long as the widget does, which is as long as the window.
    auto* watch = new Watch{std::move(dropped), {}, nullptr, false};
    g_signal_connect(widget, "drag-data-received", G_CALLBACK(on_received), watch);
    g_signal_connect(widget, "drag-drop", G_CALLBACK(on_drop), watch);
    g_object_set_data_full(G_OBJECT(widget), "crucible-drops", watch,
                           [](gpointer data) { delete static_cast<Watch*>(data); });
    return true;
}

#endif

}  // namespace crucible::gui::drops
