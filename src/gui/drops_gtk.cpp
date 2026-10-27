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
//
// The clipboard is read when its owner changes, which GTK hears of from the
// display, and both steps of a reading (what it offers, then the list of
// files) are answered later by whichever program holds it. Only the newest
// reading is said, so one answered late cannot undo a later copy.
#include "drops.hpp"

#include <memory>
#include <string_view>
#include <utility>

#include <gtk/gtk.h>

namespace crucible::gui::drops {

#if GTK_MAJOR_VERSION >= 4

bool watch(void* /*widget*/, std::function<void(std::vector<std::string>)> /*dropped*/) {
    // GTK 4's drop target is a controller that WebKit owns; a second one on
    // the same widget would take the drop from it. The page reads it instead.
    return false;
}

bool watch_clipboard(void* /*widget*/, std::function<void(std::vector<std::string>)> /*copied*/) {
    // Not written for GTK 4, whose clipboard is another API: files copied
    // there paste as their paths, as they always have.
    return false;
}

void read_clipboard(void* /*widget*/) {}

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

// --- the clipboard ------------------------------------------------------------

/// Who is told what the clipboard holds, and the number of the newest reading.
struct Clipboard {
    std::function<void(std::vector<std::string>)> copied;
    unsigned                                       newest = 0;
};

/// One reading of the clipboard. The widget is held until it is answered, so
/// what it is answered to is still there.
struct Reading {
    GtkWidget* widget;
    unsigned   number;
};

/// The formats a file manager puts files on the clipboard in, best first.
/// GNOME Files, Nemo and Thunar have their own, with "copy" or "cut" on its
/// first line, and MATE's Caja the same under its own name; Dolphin offers a
/// plain list of URIs, and most others offer one beside their own.
constexpr const char* kFileFormats[] = {"x-special/gnome-copied-files", "x-special/mate-copied-files",
                                        "text/uri-list"};

/// The files in one of those: a URI to a line, after the line saying copy or
/// cut. A URI that is not a file, a link, is left out.
std::vector<std::string> files_in(std::string_view text) {
    std::vector<std::string> paths;
    for (bool first = true; !text.empty(); first = false) {
        const std::size_t end = text.find('\n');
        std::string line(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\0')) {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#' || (first && (line == "copy" || line == "cut"))) {
            continue;
        }
        if (gchar* path = g_filename_from_uri(line.c_str(), nullptr, nullptr)) {
            paths.emplace_back(path);
            g_free(path);
        }
    }
    return paths;
}

/// Say what a reading found, unless a later one has been asked for since.
void answered(Reading* reading, std::vector<std::string> paths) {
    auto* clipboard = static_cast<Clipboard*>(g_object_get_data(G_OBJECT(reading->widget), "crucible-clipboard"));
    if (clipboard != nullptr && reading->number == clipboard->newest) {
        clipboard->copied(std::move(paths));
    }
    g_object_unref(reading->widget);
    delete reading;
}

void on_contents(GtkClipboard*, GtkSelectionData* data, gpointer user) {
    std::vector<std::string> paths;
    const gint length = data != nullptr ? gtk_selection_data_get_length(data) : -1;
    if (length > 0) {
        paths = files_in(std::string_view(reinterpret_cast<const char*>(gtk_selection_data_get_data(data)),
                                          static_cast<std::size_t>(length)));
    }
    answered(static_cast<Reading*>(user), std::move(paths));
}

void on_formats(GtkClipboard* clipboard, GdkAtom* formats, gint count, gpointer user) {
    for (const char* name : kFileFormats) {
        const GdkAtom wanted = gdk_atom_intern_static_string(name);
        for (gint i = 0; formats != nullptr && i < count; ++i) {
            if (formats[i] == wanted) {
                gtk_clipboard_request_contents(clipboard, wanted, on_contents, user);
                return;
            }
        }
    }
    answered(static_cast<Reading*>(user), {});   // text, a picture, nothing: no files
}

void read(GtkWidget* widget) {
    auto* clipboard = static_cast<Clipboard*>(g_object_get_data(G_OBJECT(widget), "crucible-clipboard"));
    if (clipboard == nullptr) {
        return;
    }
    auto* reading = new Reading{GTK_WIDGET(g_object_ref(widget)), ++clipboard->newest};
    gtk_clipboard_request_targets(gtk_widget_get_clipboard(widget, GDK_SELECTION_CLIPBOARD), on_formats, reading);
}

void on_owner_change(GtkClipboard*, GdkEvent*, gpointer widget) {
    read(static_cast<GtkWidget*>(widget));
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

bool watch_clipboard(void* widget, std::function<void(std::vector<std::string>)> copied) {
    if (widget == nullptr || !GTK_IS_WIDGET(widget)) {
        return false;
    }
    g_object_set_data_full(G_OBJECT(widget), "crucible-clipboard", new Clipboard{std::move(copied), 0},
                           [](gpointer data) { delete static_cast<Clipboard*>(data); });
    // The clipboard is the display's and outlives the widget, so the handler
    // goes when the widget does.
    g_signal_connect_object(gtk_widget_get_clipboard(GTK_WIDGET(widget), GDK_SELECTION_CLIPBOARD),
                            "owner-change", G_CALLBACK(on_owner_change), widget, static_cast<GConnectFlags>(0));
    return true;
}

void read_clipboard(void* widget) {
    if (widget != nullptr && GTK_IS_WIDGET(widget)) {
        read(static_cast<GtkWidget*>(widget));
    }
}

#endif

}  // namespace crucible::gui::drops
