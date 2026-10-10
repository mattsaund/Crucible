// SPDX-License-Identifier: MIT
//
// See apps.hpp.
#include "crucible/tools/apps.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "crucible/tools/fetch.hpp"
#include "crucible/tools/preview.hpp"
#include "crucible/util/platform.hpp"
#include "crucible/util/subprocess.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace crucible::tools::apps {
namespace {

namespace fs = std::filesystem;

/// The person's home folder, or empty.
fs::path home() {
#if defined(_WIN32)
    if (const char* profile = std::getenv("USERPROFILE"); profile != nullptr && *profile != '\0') {
        return fs::path(profile);
    }
#endif
    if (const char* value = std::getenv("HOME"); value != nullptr && *value != '\0') {
        return fs::path(value);
    }
    return {};
}

bool write_file(const fs::path& file, const std::string& text, std::string& error) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "could not write " + file.string();
        return false;
    }
    out << text;
    return static_cast<bool>(out);
}

/// Run a program to its end, its output thrown away. True when it said 0.
bool run_quietly(const std::vector<std::string>& argv) {
    util::Subprocess child;
    std::string error;
    if (!child.start(argv, {}, {}, error)) {
        return false;
    }
    std::string line;
    while (child.read_line(line)) {
    }
    return child.wait() == 0;
}

/// Copy the runner to `to`, executable.
bool place_runner(const fs::path& to, std::string& error) {
    const fs::path from = runner();
    if (from.empty()) {
        error = "this Crucible has no crucible-app beside it to make programs with -- reinstall Crucible";
        return false;
    }
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "could not copy the program into place: " + ec.message();
        return false;
    }
    fs::permissions(to, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
                            | fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace, ec);
    return true;
}

}  // namespace

namespace detail {

std::string slug(const std::string& name) {
    std::string out;
    bool dash = false;
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            if (dash && !out.empty()) {
                out += '-';
            }
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            dash = false;
        } else {
            dash = true;
        }
    }
    return out.empty() ? std::string("app") : out;
}

std::string file_name(const std::string& name) {
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
    while (!out.empty() && out.front() == ' ') {
        out.erase(out.begin());
    }
    return out.empty() ? std::string("App") : out;
}

std::string info_plist(const std::string& name, const std::string& identifier,
                       const std::string& executable) {
    const auto escaped = [](const std::string& text) {
        std::string out;
        for (const char c : text) {
            switch (c) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                default:  out += c;
            }
        }
        return out;
    };
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\">\n<dict>\n"
           "    <key>CFBundleName</key><string>" + escaped(name) + "</string>\n"
           "    <key>CFBundleDisplayName</key><string>" + escaped(name) + "</string>\n"
           "    <key>CFBundleIdentifier</key><string>" + escaped(identifier) + "</string>\n"
           "    <key>CFBundleVersion</key><string>1</string>\n"
           "    <key>CFBundleShortVersionString</key><string>1.0</string>\n"
           "    <key>CFBundleExecutable</key><string>" + escaped(executable) + "</string>\n"
           "    <key>CFBundlePackageType</key><string>APPL</string>\n"
           "    <key>LSMinimumSystemVersion</key><string>11.0</string>\n"
           "    <key>NSHighResolutionCapable</key><true/>\n"
           "</dict>\n</plist>\n";
}

std::string desktop_entry(const std::string& name, const std::filesystem::path& exec) {
    return "[Desktop Entry]\nType=Application\nName=" + name + "\nExec=\"" + exec.string()
         + "\"\nTerminal=false\nCategories=Utility;\nComment=Made with Crucible\n";
}

}  // namespace detail

std::string name_for(const fs::path& root, const std::string& page) {
    std::ifstream in(root / page, std::ios::binary);
    if (in) {
        const std::string html((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string title = tools::detail::title_of(html);
        if (!title.empty()) {
            return detail::file_name(title);
        }
    }
    return detail::file_name(root.filename().string());
}

fs::path runner() {
    const fs::path here = util::executable_path().parent_path();
#if defined(_WIN32)
    const fs::path candidate = here / "crucible-app.exe";
#else
    const fs::path candidate = here / "crucible-app";
#endif
    std::error_code ec;
    return fs::is_regular_file(candidate, ec) ? candidate : fs::path();
}

std::optional<Made> package(const fs::path& root, const std::string& page, const std::string& asked,
                            std::string& error) {
    const std::string name = detail::file_name(asked.empty() ? name_for(root, page) : asked);
    const preview::Bundle bundle = preview::bundle(root, page, 24U << 20);
    if (!bundle.ok) {
        error = bundle.error.empty() ? page + " could not be read" : bundle.error;
        return std::nullopt;
    }
    const nlohmann::json about{{"name", name}, {"width", 1100}, {"height", 760}, {"page", page}};
    const fs::path base = home();
    if (base.empty()) {
        error = "could not tell where your home folder is";
        return std::nullopt;
    }

    Made made;
    made.name = name;

    // Somewhere else entirely, when asked: the tests, and anybody who keeps
    // their programs on another drive. Nothing is registered with the
    // system then -- no Launchpad, no Start menu -- only the files made.
    if (const char* elsewhere = std::getenv("CRUCIBLE_APPS_DIR"); elsewhere != nullptr && *elsewhere != '\0') {
        const fs::path folder = fs::path(elsewhere) / name;
        std::error_code ec;
        fs::remove_all(folder, ec);
#if defined(_WIN32)
        const fs::path exe = folder / (name + ".exe");
#else
        const fs::path exe = folder / detail::slug(name);
#endif
        if (!place_runner(exe, error) || !write_file(folder / "app" / "index.html", bundle.html, error)
            || !write_file(folder / "app" / "app.json", about.dump(2), error)) {
            return std::nullopt;
        }
        made.program = folder;
        made.launch  = exe;
        made.where   = "in " + folder.string();
        return made;
    }
#if defined(__APPLE__)
    // ~/Applications: a person's own Applications folder, which Launchpad and
    // Spotlight look in, and which needs no administrator to write to.
    const fs::path bundle_dir = base / "Applications" / (name + ".app");
    std::error_code ec;
    fs::remove_all(bundle_dir, ec);
    const fs::path contents = bundle_dir / "Contents";
    const std::string executable = detail::slug(name);
    if (!place_runner(contents / "MacOS" / executable, error)
        || !write_file(contents / "Resources" / "app" / "index.html", bundle.html, error)
        || !write_file(contents / "Resources" / "app" / "app.json", about.dump(2), error)
        || !write_file(contents / "Info.plist",
                       detail::info_plist(name, "dev.crucible.made." + detail::slug(name), executable), error)) {
        return std::nullopt;
    }
    // Signed as it is now, the bundle and the binary in it: a Mac will not
    // run a program whose signature no longer matches what is on disk, and
    // the copy is a new bundle around an old signature.
    run_quietly({"codesign", "--force", "--deep", "--sign", "-", "--timestamp=none", bundle_dir.string()});
    // Told about at once, so it is in Launchpad and Spotlight now rather than
    // whenever the index next looks.
    run_quietly({"/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/"
                 "Support/lsregister", "-f", bundle_dir.string()});
    made.program = bundle_dir;
    made.launch  = bundle_dir;
    made.where   = "in Applications, in your home folder -- and in Launchpad";
#elif defined(_WIN32)
    fs::path programs;
    if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && *local != '\0') {
        programs = fs::path(local) / "Programs";
    } else {
        programs = base / "AppData" / "Local" / "Programs";
    }
    const fs::path folder = programs / name;
    std::error_code ec;
    fs::remove_all(folder, ec);
    const fs::path exe = folder / (name + ".exe");
    if (!place_runner(exe, error) || !write_file(folder / "app" / "index.html", bundle.html, error)
        || !write_file(folder / "app" / "app.json", about.dump(2), error)) {
        return std::nullopt;
    }
    // A Start menu entry and one on the desktop, made by the shell's own COM
    // object -- the way an installer would, with nothing to install.
    const auto shortcut = [&exe, &folder](const std::string& in) {
        return "$s=(New-Object -ComObject WScript.Shell).CreateShortcut(\"" + in + "\");"
               "$s.TargetPath=\"" + exe.string() + "\";$s.WorkingDirectory=\"" + folder.string() + "\";$s.Save();";
    };
    const std::string script =
        shortcut("$env:APPDATA\\Microsoft\\Windows\\Start Menu\\Programs\\" + name + ".lnk")
        + shortcut("$([Environment]::GetFolderPath('Desktop'))\\" + name + ".lnk");
    run_quietly({"powershell", "-NoProfile", "-NonInteractive", "-Command", script});
    made.program = folder;
    made.launch  = exe;
    made.where   = "in the Start menu and on the desktop";
#else
    fs::path share;
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
        share = fs::path(xdg);
    } else {
        share = base / ".local" / "share";
    }
    const std::string slug = detail::slug(name);
    const fs::path folder = share / "crucible-apps" / slug;
    std::error_code ec;
    fs::remove_all(folder, ec);
    const fs::path exe = folder / slug;
    if (!place_runner(exe, error) || !write_file(folder / "app" / "index.html", bundle.html, error)
        || !write_file(folder / "app" / "app.json", about.dump(2), error)
        || !write_file(share / "applications" / (slug + ".desktop"), detail::desktop_entry(name, exe), error)) {
        return std::nullopt;
    }
    run_quietly({"update-desktop-database", (share / "applications").string()});
    made.program = folder;
    made.launch  = exe;
    made.where   = "in your applications menu";
#endif
    return made;
}

bool open(const fs::path& launch, std::string& error) {
    std::error_code ec;
    if (!fs::exists(launch, ec)) {
        error = launch.string() + " is not there any more";
        return false;
    }
    // Started the way the person would start it, so that it is theirs and not
    // a child of Crucible's: closing Crucible must not close their budget.
#if defined(__APPLE__)
    const bool started = run_quietly({"open", launch.string()});
#elif defined(_WIN32)
    const std::wstring file = launch.wstring();
    const std::wstring folder = launch.parent_path().wstring();
    const bool started = reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", file.c_str(), nullptr,
                                                                   folder.c_str(), SW_SHOWNORMAL)) > 32;
#else
    // A session of its own, so it outlives Crucible and anything Crucible's
    // process group is sent.
    const bool started = run_quietly({"/bin/sh", "-c", "setsid \"$0\" >/dev/null 2>&1 </dev/null &",
                                      launch.string()});
#endif
    if (!started) {
        error = "could not open " + launch.string();
    }
    return started;
}

}  // namespace crucible::tools::apps
