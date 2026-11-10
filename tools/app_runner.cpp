// SPDX-License-Identifier: MIT
//
// crucible-app -- a program Crucible made, running as a program of its own.
//
// Crucible makes the programs people ask it for as pages -- HTML, CSS and
// JavaScript, the one kind of program every machine runs with nothing
// installed -- and shows them in its preview while it builds them. Packaged,
// a page needs a window of its own and somewhere to keep its data, and this is
// both. A copy of this binary is the program: its page and its name sit beside
// it, in app/index.html and app/app.json -- inside Contents/Resources on a Mac
// -- and src/tools/apps.cpp is where Crucible puts the copy and tells the
// operating system about it.
//
// The page is given one thing: window.crucible.load() and
// window.crucible.save(data), the two calls Crucible's preview answers too,
// kept here as a JSON file in the person's own data folder. Not beside the
// program, which an update replaces: the budget somebody typed in last month
// is theirs, and outlives the version of the program that holds it. And
// localStorage, which a page handed over as a string has none of, stood in
// for as the preview stands in for it, and kept in a file beside that one.
//
// On Linux the page is not handed over as a string but served, from an
// address of the program's own, so that location.reload() has somewhere to
// go back to. See serve().
//
// Nothing else of Crucible is in it -- no models, no engine -- so it is small,
// starts at once, and runs on a machine Crucible was never installed on.
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>

#include "crucible/tools/page_storage.hpp"
#include "webkit_env.hpp"

#include <nlohmann/json.hpp>
#include <webview/webview.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#else
#include <climits>
#include <unistd.h>
#endif

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

/// Where this binary is.
fs::path executable() {
#if defined(_WIN32)
    wchar_t buffer[32768];
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return length == 0 ? fs::path() : fs::path(std::wstring(buffer, length));
#elif defined(__APPLE__)
    char buffer[PATH_MAX];
    std::uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) != 0) {
        return {};
    }
    std::error_code ec;
    const fs::path found = fs::weakly_canonical(fs::path(buffer), ec);
    return ec ? fs::path(buffer) : found;
#else
    char buffer[PATH_MAX];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    return length <= 0 ? fs::path() : fs::path(std::string(buffer, static_cast<std::size_t>(length)));
#endif
}

/// The app's folder: app/ beside the binary, or Resources/app in a Mac bundle.
fs::path app_folder() {
    const fs::path here = executable().parent_path();
    std::error_code ec;
    for (const fs::path& candidate : {here / "app", here.parent_path() / "Resources" / "app"}) {
        if (fs::is_regular_file(candidate / "index.html", ec)) {
            return candidate;
        }
    }
    return here / "app";
}

std::string read_all(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return in ? std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>())
              : std::string();
}

/// A name a folder can have: the app's name with anything a file system
/// objects to taken out.
std::string folder_name(const std::string& name) {
    std::string out;
    for (const char c : name) {
        if (std::string_view("<>:\"/\\|?*").find(c) == std::string_view::npos
            && static_cast<unsigned char>(c) >= 32) {
            out += c;
        }
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
        out.pop_back();
    }
    return out.empty() ? std::string("Crucible app") : out;
}

/// Where the person's data for this app is kept: their own data folder, the
/// one each platform has for exactly this.
fs::path data_file(const std::string& name) {
    fs::path base;
#if defined(_WIN32)
    if (const char* roaming = std::getenv("APPDATA"); roaming != nullptr && *roaming != '\0') {
        base = fs::path(roaming);
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home != nullptr) {
        base = fs::path(home) / "Library" / "Application Support";
    }
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
        base = fs::path(xdg);
    } else if (const char* home = std::getenv("HOME"); home != nullptr) {
        base = fs::path(home) / ".local" / "share";
    }
#endif
    if (base.empty()) {
        base = app_folder();
    }
    return base / folder_name(name) / "data.json";
}

/// The page's window.crucible: the same two calls Crucible's preview gives a
/// page, answered here from the data file.
constexpr const char* kBridge = R"JS(
window.crucible = {
  native: true,
  load: function () { return window.__crucibleLoad(); },
  save: function (data) { return window.__crucibleSave(data === undefined ? null : data); }
};
)JS";

/// `value` written to `file` whole. Written beside and renamed over, so a
/// program closed mid-save leaves the last whole copy rather than half of
/// the new one.
bool write_whole(const fs::path& file, const json& value) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path part = file;
    part += ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << value.dump(2, ' ', false, json::error_handler_t::replace);
    }
    fs::rename(part, file, ec);
    return !ec;
}

/// What a page that is missing is shown instead.
std::string missing_page(const std::string& name) {
    return "<!doctype html><title>" + name + "</title><body style='font:16px sans-serif;"
           "padding:2rem'>This program's page is missing. Make it an app again from Crucible.";
}

#if defined(WEBVIEW_GTK)
/// The page as it is served: the app's own, with the stand-ins for its
/// storage first, holding `local` as storage.json has it and `session` as the
/// program has held it since it started, and handing what they hold to `keep`.
std::string document_of(const std::string& page, const json& local, const json& session, const char* keep) {
    const json kept{{"local", local.is_object() ? local : json::object()},
                    {"session", session.is_object() ? session : json::object()}};
    return crucible::tools::with_script_first(
        page, crucible::tools::page_storage_script(kept.dump(-1, ' ', false, json::error_handler_t::replace), keep));
}

/// Where WebKitGTK is given the page, from a scheme of the program's own.
///
/// A page handed to it as a string is a document at about:blank, and a
/// location.reload() goes back to about:blank: an empty window. Asked for at
/// an address, the page is served again on a reload, holding what its storage
/// holds by then rather than what it held when the program started.
constexpr const char* kScheme  = "crucible-app";
constexpr const char* kAddress = "crucible-app://app/";

/// Whatever the scheme is asked for, made into what it answers with: the
/// page for the address itself, nothing for any other.
struct Served {
    std::function<std::string()> document;
};

void answer(WebKitURISchemeRequest* request, const Served& served) {
    const std::string path = webkit_uri_scheme_request_get_path(request) != nullptr
                                 ? webkit_uri_scheme_request_get_path(request) : "";
    if (path != "/" && path != "/index.html") {
        GError* error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "not part of this program");
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }
    const std::string document = served.document();
    GBytes*       bytes  = g_bytes_new(document.data(), document.size());
    GInputStream* stream = g_memory_input_stream_new_from_bytes(bytes);
    // The encoding said, as a page handed over as a string was read in it.
    webkit_uri_scheme_request_finish(request, stream, static_cast<gint64>(document.size()),
                                     "text/html; charset=utf-8");
    g_object_unref(stream);
    g_bytes_unref(bytes);
}

/// The scheme's handler. It answers once whatever the page asked of the
/// program before it went is done: a binding runs in an idle callback of the
/// webview's (its dispatch), and a page that saves and reloads in one click
/// must be served what it saved. Queued behind those, at their priority.
void serve(WebKitURISchemeRequest* request, gpointer data) {
    struct Waiting {
        WebKitURISchemeRequest* request;
        const Served*           served;
    };
    g_object_ref(request);
    g_idle_add_full(
        G_PRIORITY_HIGH_IDLE,
        [](gpointer waiting) -> gboolean {
            const auto* one = static_cast<Waiting*>(waiting);
            answer(one->request, *one->served);
            return G_SOURCE_REMOVE;
        },
        new Waiting{request, static_cast<const Served*>(data)},
        [](gpointer waiting) {
            const auto* one = static_cast<Waiting*>(waiting);
            g_object_unref(one->request);
            delete one;
        });
}
#endif

}  // namespace

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
#else
int main() {
#endif
#if !defined(_WIN32) && !defined(__APPLE__)
    crucible::gui::avoid_nvidia_dmabuf_renderer();
#endif
    const fs::path folder = app_folder();
    const json about = json::parse(read_all(folder / "app.json"), nullptr, false);
    const std::string name = about.is_object() ? about.value("name", std::string("App")) : "App";
    const int width  = about.is_object() ? about.value("width", 1100) : 1100;
    const int height = about.is_object() ? about.value("height", 760) : 760;
    const fs::path data = data_file(name);

    // localStorage, stood in for (crucible/tools/page_storage.hpp) and kept in
    // a file of its own beside the data, so neither can write over the other;
    // sessionStorage held here for as long as the program is open, as a
    // browser holds it, so that a page served again on a reload finds it.
    // Made before the window, which runs what is still queued for it as it
    // closes, and so outliving it.
    const fs::path storage = data.parent_path() / "storage.json";
    json           session = json::object();
    const auto     keep    = "function (kind, items) { window.__crucibleKeep(kind, items); }";
#if defined(WEBVIEW_GTK)
    // Served, with the stand-ins inside the page, holding what the storage
    // holds each time it is asked for.
    Served served{[&] {
        const std::string page = read_all(folder / "index.html");
        return page.empty() ? missing_page(name)
                            : document_of(page, json::parse(read_all(storage), nullptr, false), session, keep);
    }};
#endif

    webview::webview view(/*debug=*/false, nullptr);
    view.set_title(name);
    view.set_size(width, height, WEBVIEW_HINT_NONE);

    view.bind("__crucibleLoad", [data](const std::string&) -> std::string {
        const json saved = json::parse(read_all(data), nullptr, false);
        return saved.is_discarded() ? std::string("null")
                                    : saved.dump(-1, ' ', false, json::error_handler_t::replace);
    });
    view.bind("__crucibleSave", [data](const std::string& arguments) -> std::string {
        const json given = json::parse(arguments, nullptr, false);
        return write_whole(data, given.is_array() && !given.empty() ? given[0] : json(nullptr)) ? "true"
                                                                                               : "false";
    });
    view.init(kBridge);

    view.bind("__crucibleKeep", [storage, &session](const std::string& arguments) -> std::string {
        const json given = json::parse(arguments, nullptr, false);
        if (!given.is_array() || given.size() < 2 || !given[0].is_string() || !given[1].is_object()) {
            return "false";
        }
        if (given[0] == "session") {
            session = given[1];
            return "true";
        }
        return given[0] == "local" && write_whole(storage, given[1]) ? "true" : "false";
    });

    // Handed over as a string, with the stand-ins put in before it as it
    // starts, holding what the storage held then. Not inside the page itself:
    // WebView2 takes a page of two megabytes at most as a string, and a page's
    // storage alone can be more.
    const auto as_string = [&] {
        const json held = json::parse(read_all(storage), nullptr, false);
        const json kept{{"local", held.is_object() ? held : json::object()}, {"session", json::object()}};
        view.init(crucible::tools::page_storage_script(kept.dump(-1, ' ', false, json::error_handler_t::replace),
                                                       keep));
        const std::string page = read_all(folder / "index.html");
        view.set_html(page.empty() ? missing_page(name) : page);
    };

#if defined(WEBVIEW_GTK)
    const auto widget = view.widget();
    if (widget.ok()) {
        webkit_web_context_register_uri_scheme(webkit_web_view_get_context(WEBKIT_WEB_VIEW(widget.value())),
                                               kScheme, serve, &served, nullptr);
        view.navigate(kAddress);
    } else {
        as_string();
    }
#else
    as_string();
#endif
    view.run();
    return 0;
}
