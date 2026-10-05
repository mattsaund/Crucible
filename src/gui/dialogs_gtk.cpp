// SPDX-License-Identifier: MIT
//
// The file dialog on Linux and the BSDs: GtkFileChooserNative.
//
// "Native" is GTK's word and it means something specific. Where the desktop
// has a file-chooser portal -- GNOME, KDE, anything running under Flatpak --
// this shows the desktop's own dialog out of process; where it does not, it
// shows GTK's. Either way it is the one call, and the choice is the
// desktop's rather than this file's.
//
// Written against the part of the API that GTK 3 and GTK 4 share, because
// the webview builds against whichever the distribution ships.
#include "dialogs.hpp"

#include <memory>
#include <utility>

#include <gtk/gtk.h>

namespace crucible::gui::dialogs {
namespace {

/// What the response handler needs, kept alive across the wait for it.
struct Pending {
    std::function<void(Answer)> done;
};

void on_response(GtkNativeDialog* dialog, gint response, gpointer data) {
    const std::unique_ptr<Pending> pending(static_cast<Pending*>(data));

    Answer answer;
    if (response == GTK_RESPONSE_ACCEPT) {
        // GFiles rather than filenames: they are what both GTK versions
        // hand back, and they are right for a path that is not valid UTF-8.
        // The list is a GListModel in GTK 4 and a GSList in GTK 3.
        const auto take = [&answer](GFile* file) {
            if (char* path = g_file_get_path(file)) {
                answer.paths.emplace_back(path);
                g_free(path);
            }
        };
#if GTK_MAJOR_VERSION >= 4
        GListModel* files = gtk_file_chooser_get_files(GTK_FILE_CHOOSER(dialog));
        for (guint i = 0; files != nullptr && i < g_list_model_get_n_items(files); ++i) {
            auto* file = static_cast<GFile*>(g_list_model_get_item(files, i));
            take(file);
            g_object_unref(file);
        }
        if (files != nullptr) {
            g_object_unref(files);
        }
#else
        GSList* files = gtk_file_chooser_get_files(GTK_FILE_CHOOSER(dialog));
        for (GSList* at = files; at != nullptr; at = at->next) {
            take(static_cast<GFile*>(at->data));
        }
        g_slist_free_full(files, g_object_unref);
#endif
        if (!answer.paths.empty()) {
            answer.path = answer.paths.front();
        }
    }
    g_object_unref(dialog);
    pending->done(std::move(answer));
}

}  // namespace

void pick(void* window, const Request& request, std::function<void(Answer)> done) {
    GtkFileChooserNative* native = gtk_file_chooser_native_new(
        request.title.empty() ? nullptr : request.title.c_str(),
        window != nullptr ? GTK_WINDOW(window) : nullptr,
        request.folder ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER : GTK_FILE_CHOOSER_ACTION_OPEN,
        request.folder ? "_Choose" : "_Open", "_Cancel");
    if (native == nullptr) {
        Answer none;
        none.supported = false;
        done(std::move(none));
        return;
    }
    GtkFileChooser* chooser = GTK_FILE_CHOOSER(native);

    if (!request.start.empty()) {
#if GTK_MAJOR_VERSION >= 4
        GFile* start = g_file_new_for_path(request.start.c_str());
        gtk_file_chooser_set_current_folder(chooser, start, nullptr);
        g_object_unref(start);
#else
        gtk_file_chooser_set_current_folder(chooser, request.start.c_str());
#endif
    }

    if (!request.folder && request.multiple) {
        gtk_file_chooser_set_select_multiple(chooser, TRUE);
    }

    if (!request.folder && !request.extensions.empty()) {
        GtkFileFilter* wanted = gtk_file_filter_new();
        gtk_file_filter_set_name(wanted, request.filter_name.empty()
                                             ? "Matching files" : request.filter_name.c_str());
        for (const std::string& extension : request.extensions) {
            gtk_file_filter_add_pattern(wanted, ("*" + extension).c_str());
        }
        gtk_file_chooser_add_filter(chooser, wanted);

        // And a way past it. A model with an unusual extension is still a
        // model, and a filter with no "all files" beside it is a dead end.
        GtkFileFilter* any = gtk_file_filter_new();
        gtk_file_filter_set_name(any, "All files");
        gtk_file_filter_add_pattern(any, "*");
        gtk_file_chooser_add_filter(chooser, any);
    }

    // Shown rather than run. gtk_native_dialog_run spins a nested main loop,
    // which GTK 4 removed and which, inside a webview's message handler, is
    // an invitation to re-enter it.
    gtk_native_dialog_set_modal(GTK_NATIVE_DIALOG(native), TRUE);
    g_signal_connect(native, "response", G_CALLBACK(on_response),
                     new Pending{std::move(done)});
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(native));
}

}  // namespace crucible::gui::dialogs
