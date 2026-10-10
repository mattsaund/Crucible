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
// is theirs, and outlives the version of the program that holds it.
//
// Nothing else of Crucible is in it -- no models, no engine -- so it is small,
// starts at once, and runs on a machine Crucible was never installed on.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>

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

}  // namespace

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
#else
int main() {
#endif
    const fs::path folder = app_folder();
    const json about = json::parse(read_all(folder / "app.json"), nullptr, false);
    const std::string name = about.is_object() ? about.value("name", std::string("App")) : "App";
    const int width  = about.is_object() ? about.value("width", 1100) : 1100;
    const int height = about.is_object() ? about.value("height", 760) : 760;
    const fs::path data = data_file(name);

    webview::webview view(/*debug=*/false, nullptr);
    view.set_title(name);
    view.set_size(width, height, WEBVIEW_HINT_NONE);

    view.bind("__crucibleLoad", [data](const std::string&) -> std::string {
        const json saved = json::parse(read_all(data), nullptr, false);
        return saved.is_discarded() ? std::string("null")
                                    : saved.dump(-1, ' ', false, json::error_handler_t::replace);
    });
    view.bind("__crucibleSave", [data](const std::string& arguments) -> std::string {
        // Written beside and renamed over, so a program closed mid-save leaves
        // the last whole copy rather than half of the new one.
        const json given = json::parse(arguments, nullptr, false);
        const json value = given.is_array() && !given.empty() ? given[0] : json(nullptr);
        std::error_code ec;
        fs::create_directories(data.parent_path(), ec);
        const fs::path part = data.string() + ".part";
        {
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            if (!out) {
                return "false";
            }
            out << value.dump(2, ' ', false, json::error_handler_t::replace);
        }
        fs::rename(part, data, ec);
        return ec ? "false" : "true";
    });
    view.init(kBridge);

    const std::string page = read_all(folder / "index.html");
    if (page.empty()) {
        view.set_html("<!doctype html><title>" + name + "</title><body style='font:16px sans-serif;"
                      "padding:2rem'>This program's page is missing. Make it an app again from Crucible.");
    } else {
        view.set_html(page);
    }
    view.run();
    return 0;
}
