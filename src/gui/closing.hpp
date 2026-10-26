// SPDX-License-Identifier: MIT
//
// Closing the window over work that is not saved.
//
// The Build tab's code pane holds what is typed in it until it is saved, and
// only the page knows about it. Closing the window used to drop it without a
// word. So the platform's close is caught here before it happens, and the
// window decides (window.cpp): with nothing unsaved it closes at once, as it
// always did; otherwise it stays open while the page asks.
//
// Caught where each platform asks whether a window may close: GTK's
// delete-event, WM_CLOSE on Windows, and on a Mac windowShouldClose: and
// applicationShouldTerminate:, added to the delegates the webview made
// through the Objective-C runtime, the way dialogs_mac.cpp talks to AppKit.
#pragma once

#include <functional>

namespace crucible::gui::closing {

/// Ask `may_close`, on the window's thread, before `window` -- the webview's
/// own window -- closes at the person's asking; false keeps it open. One
/// window per process. Returns false when this platform's window cannot be
/// watched, and it then closes as it always has.
bool watch(void* window, std::function<bool()> may_close);

}  // namespace crucible::gui::closing
