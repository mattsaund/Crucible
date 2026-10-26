// SPDX-License-Identifier: MIT
//
// Closing on a Mac. The title bar's button asks the window's delegate
// windowShouldClose:, and the Dock's Quit asks the application's delegate
// applicationShouldTerminate:. The webview made both delegates and answers
// neither, so the answer is added to their classes here -- through the
// Objective-C runtime, as dialogs_mac.cpp talks to AppKit and the webview
// does itself. Neither is asked when the webview closes the window on its
// way out.
#include "closing.hpp"

#include <utility>

#include <objc/message.h>
#include <objc/runtime.h>

namespace crucible::gui::closing {
namespace {

// One window per process, so one answer.
std::function<bool()> g_may_close;

/// objc_msgSend, called through a pointer of the type the method has.
template <typename Result, typename... Arguments>
Result send(id receiver, const char* selector, Arguments... arguments) {
    using Function = Result (*)(id, SEL, Arguments...);
    return reinterpret_cast<Function>(objc_msgSend)(receiver, sel_registerName(selector),
                                                    arguments...);
}

bool may_close() { return !g_may_close || g_may_close(); }

/// NSTerminateCancel and NSTerminateNow, spelled as numbers because the
/// header that names them is an Objective-C one.
constexpr unsigned long kTerminateCancel = 0;
constexpr unsigned long kTerminateNow    = 1;

/// Give the class of `object` a method for `selector`. False when there is
/// no object, or its class answers that already and is left to.
bool answer(id object, const char* selector, IMP how, const char* types) {
    return object != nullptr
        && class_addMethod(object_getClass(object), sel_registerName(selector), how, types);
}

}  // namespace

bool watch(void* window, std::function<bool()> may_close_now) {
    const id shown = static_cast<id>(window);
    if (shown == nullptr) {
        return false;
    }
    g_may_close = std::move(may_close_now);
    const bool watched =
        answer(send<id>(shown, "delegate"), "windowShouldClose:",
               reinterpret_cast<IMP>(+[](id, SEL, id) -> BOOL { return may_close() ? YES : NO; }),
               "c@:@");
    // Quitting ends the application without asking the window anything.
    const id application = send<id>(reinterpret_cast<id>(objc_getClass("NSApplication")),
                                    "sharedApplication");
    answer(send<id>(application, "delegate"), "applicationShouldTerminate:",
           reinterpret_cast<IMP>(+[](id, SEL, id) -> unsigned long {
               return may_close() ? kTerminateNow : kTerminateCancel;
           }),
           "Q@:@");
    return watched;
}

}  // namespace crucible::gui::closing
