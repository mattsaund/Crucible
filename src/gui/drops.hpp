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
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace crucible::gui::drops {

/// Call `dropped` with the paths of whatever is dropped on `widget`, the
/// webview's own widget, on the window's thread. Returns false when this
/// platform's webview cannot say -- and `dropped` is never called.
bool watch(void* widget, std::function<void(std::vector<std::string>)> dropped);

}  // namespace crucible::gui::drops
