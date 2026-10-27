// SPDX-License-Identifier: MIT
//
// Where files dropped on the window came from.
//
// A page is handed what is dropped on it as File objects -- their names and
// contents, never their paths -- and WebKitGTK hands it less than that: no
// files at all, and a list of URIs with the file:// ones taken out. Under
// the page, though, the drop arrives at the window as the desktop sent it, a
// list of paths, and where that can be read the page is told the paths and
// attaches the files themselves, folders and all, with nothing copied.
//
// GTK 3 is where that is read: the webview there is a GtkWidget, and a drop
// on it is a signal anything can listen to. WebView2 and WKWebView keep
// theirs inside, and GTK 4's drop target is WebKit's own; on those the page
// reads the files it is handed and keeps a copy (see ui/attach.js and
// attach::keep_dropped).
//
// Files copied in a file manager and pasted are the same story told again.
// WebKitGTK pastes them as their paths, as text, and hands the page nothing
// else; the clipboard itself says which files they are, in a format of the
// file manager's own beside the text, and GTK 3 is where that can be read.
// A cut is read as a copy: attaching a file never moves it, and the file
// manager that cut it still has it to move.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace crucible::gui::drops {

/// Call `dropped` with the paths of whatever is dropped on `widget`, the
/// webview's own widget, on the window's thread. Returns false when this
/// platform's webview cannot say -- and `dropped` is never called.
bool watch(void* widget, std::function<void(std::vector<std::string>)> dropped);

/// Call `copied` with the paths of the files on the clipboard of `widget`'s
/// display whenever what is on it changes, on the window's thread: the files
/// a file manager copied or cut, and none when it holds anything else.
/// Returns false when this platform's webview cannot say, and then `copied`
/// is never called.
bool watch_clipboard(void* widget, std::function<void(std::vector<std::string>)> copied);

/// Call `copied` with what is on the clipboard now, changed or not. Does
/// nothing where it is not watched.
void read_clipboard(void* widget);

}  // namespace crucible::gui::drops
