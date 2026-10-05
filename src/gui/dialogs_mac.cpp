// SPDX-License-Identifier: MIT
//
// The file dialog on macOS: NSOpenPanel.
//
// Through the Objective-C runtime rather than in an Objective-C++ file, which
// is how the webview underneath this talks to Cocoa as well. It keeps the
// build to one language -- there is no .mm anywhere in Crucible, and one
// dialog is not the reason to add the toolchain settings for one.
//
// The price is that a message is a string here and the compiler cannot check
// it. So there are few of them, each is one AppKit has had since 10.6, and
// each is spelled once, below.
#include "dialogs.hpp"

#include <utility>

#include <objc/message.h>
#include <objc/runtime.h>

namespace crucible::gui::dialogs {
namespace {

/// objc_msgSend has no one signature: it is called through a pointer of the
/// type the method actually has. These are the shapes used below.
template <typename Result, typename... Arguments>
Result send(id receiver, const char* selector, Arguments... arguments) {
    using Function = Result (*)(id, SEL, Arguments...);
    return reinterpret_cast<Function>(objc_msgSend)(receiver, sel_registerName(selector),
                                                    arguments...);
}

id class_named(const char* name) {
    return reinterpret_cast<id>(objc_getClass(name));
}

id ns_string(const std::string& text) {
    return send<id, const char*>(class_named("NSString"), "stringWithUTF8String:",
                                 text.c_str());
}

/// NSModalResponseOK. Spelled as a number because the header that names it is
/// an Objective-C one.
constexpr long kModalResponseOK = 1;

}  // namespace

void pick(void* /*window*/, const Request& request, std::function<void(Answer)> done) {
    Answer answer;

    const id panel = send<id>(class_named("NSOpenPanel"), "openPanel");
    if (panel == nullptr) {
        answer.supported = false;
        done(std::move(answer));
        return;
    }

    const BOOL folder = request.folder ? YES : NO;
    send<void, BOOL>(panel, "setCanChooseFiles:", folder == YES ? NO : YES);
    send<void, BOOL>(panel, "setCanChooseDirectories:", folder);
    send<void, BOOL>(panel, "setAllowsMultipleSelection:",
                     request.multiple && folder == NO ? YES : NO);
    send<void, BOOL>(panel, "setCanCreateDirectories:", YES);

    if (!request.title.empty()) {
        // A sheet has no title bar on current macOS; the message line is
        // where an open panel says what it is for.
        send<void, id>(panel, "setMessage:", ns_string(request.title));
    }
    if (!request.start.empty()) {
        const id start = send<id, id, BOOL>(class_named("NSURL"), "fileURLWithPath:isDirectory:",
                                            ns_string(request.start), YES);
        if (start != nullptr) {
            send<void, id>(panel, "setDirectoryURL:", start);
        }
    }
    // No file-type filter. The API for one changed in macOS 11 and again in
    // 12, and a panel that shows every file is a smaller problem than one
    // that will not compile on half the machines it is built for.

    // Modal, with its own run loop while it is up.
    if (send<long>(panel, "runModal") == kModalResponseOK) {
        // URLs, which holds the one URL as well when only one could be
        // chosen.
        const id urls = send<id>(panel, "URLs");
        const unsigned long count = urls != nullptr ? send<unsigned long>(urls, "count") : 0;
        for (unsigned long i = 0; i < count; ++i) {
            const id url  = send<id, unsigned long>(urls, "objectAtIndex:", i);
            const id path = url != nullptr ? send<id>(url, "path") : nullptr;
            if (path != nullptr) {
                if (const char* text = send<const char*>(path, "UTF8String")) {
                    answer.paths.emplace_back(text);
                }
            }
        }
        if (!answer.paths.empty()) {
            answer.path = answer.paths.front();
        }
    }
    done(std::move(answer));
}

}  // namespace crucible::gui::dialogs
