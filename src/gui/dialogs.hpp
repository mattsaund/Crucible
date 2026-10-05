// SPDX-License-Identifier: MIT
//
// The platform's own file dialog.
//
// Crucible drew its own folder picker for a long time, and it is still there
// as the fallback. But the dialog a desktop already has knows things a
// home-made one never will -- the bookmarks in its sidebar, the network
// shares that are mounted, the drive that was plugged in a minute ago, the
// last place you were -- and it is the one the person at the keyboard has
// already learned.
//
// One function with three implementations: GtkFileChooserNative on Linux
// (which hands off to the desktop's portal where there is one, so a KDE
// session gets KDE's dialog), IFileOpenDialog on Windows, NSOpenPanel on
// macOS. Each is in its own file and the build takes the one that applies.
// Beside it, the other way round: a folder shown in the desktop's own file
// browser.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace crucible::gui::dialogs {

/// What to ask for.
struct Request {
    bool        folder = false;   ///< a folder rather than a file
    std::string title;            ///< what the dialog is for, in its title bar
    std::string start;            ///< the directory to open in, or empty

    /// The kind of file wanted: a name for the filter and the extensions it
    /// passes, each with its dot. Ignored when picking a folder, and empty
    /// means any file.
    std::string              filter_name;
    std::vector<std::string> extensions;

    /// More than one may be chosen. Files only: every platform's folder
    /// dialog chooses one.
    bool multiple = false;
};

/// What came back.
struct Answer {
    /// False when this build or this desktop has no dialog to show. The page
    /// falls back to its own picker, which is the only time it is used.
    bool supported = true;

    /// The path chosen. Empty when the dialog was dismissed.
    std::string path;

    /// Every path chosen, when more than one could be; `path` is the first.
    std::vector<std::string> paths;
};

/// Show the dialog over `window`, which is the webview's native window handle
/// (a GtkWindow*, an HWND or an NSWindow*).
///
/// Call on the thread that owns the window. `done` is called on that same
/// thread exactly once -- before this returns on the platforms whose dialogs
/// are modal loops, and some time after it on the one whose is not.
void pick(void* window, const Request& request, std::function<void(Answer)> done);

/// Open `folder` in the platform's file browser -- whichever the desktop has
/// on Linux, Finder, Explorer. Empty when it was shown, or what went wrong.
///
/// A folder, never a file: handed a file, each of these opens it in the
/// application it belongs to, and for a script that is running it. The
/// caller makes sure it is a directory. Call on the window's thread.
std::string show_folder(const std::string& folder);

}  // namespace crucible::gui::dialogs
