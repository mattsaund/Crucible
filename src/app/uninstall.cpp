// SPDX-License-Identifier: MIT
//
// Removing Crucible.
//
// One question, and it takes everything: both programs, the libraries under
// them, the application-menu entry, the configuration, the folder-trust list,
// the models directory, the compute runtimes built into it, the project
// history and the caches.
//
// It used to ask three -- programs, then configuration, then models and data --
// so that a partial uninstall was possible. In practice that made a full
// removal something you had to agree to three times and a half-removal the
// easiest outcome to reach by accident, and it left the one thing an
// uninstaller is for, "this machine is now as it was", as the least likely
// result. Uninstall means uninstall. The list is printed before the question,
// so what is about to go is on screen while it is still being asked.
#include "crucible/app/uninstall.hpp"

#include "crucible/util/platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "crucible/llm/model_catalog.hpp"
#include "crucible/config/paths.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/subprocess.hpp"

namespace crucible {
namespace {

/// The path of the running executable. Uninstalling the binary that is asking
/// the question is the whole point, so resolve it rather than guessing at
/// /usr/local/bin.
///
/// This used to read /proc/self/exe directly, which is Linux -- on macOS it
/// returned nothing and the uninstaller could not find the binary it was being
/// asked to remove.
std::filesystem::path own_path() {
    return util::executable_path();
}

std::uintmax_t directory_size(const std::filesystem::path& dir) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end;
         it.increment(ec)) {
        if (ec) {
            break;
        }
        std::error_code file_ec;
        if (it->is_regular_file(file_ec) && !file_ec) {
            total += std::filesystem::file_size(it->path(), file_ec);
        }
    }
    return total;
}

/// The question.
///
/// A question that cannot be read is not an answer of "no". Treating it as one
/// is how `curl ... | bash -s -- --uninstall` came to print "Done." having
/// removed nothing: stdin there is the installer script still being read, so
/// the question swallowed a line of shell and then hit end of input -- and that
/// was recorded as the user declining. install.sh now hands the binary the
/// terminal, and an input that gives out anyway is reported rather than
/// quietly deciding to keep everything.
struct Prompter {
    bool assume_yes   = false;
    bool unanswerable = false;

    bool ask(const std::string& question, bool default_yes) {
        if (assume_yes) {
            return true;
        }
        if (unanswerable) {
            return false;
        }
        // Anything that is not yes, no, or the default is not an answer, and it
        // is asked again rather than filed as "no". A typed mistake costs a
        // re-prompt; a stdin that is not answers at all -- the installer script
        // arriving down the curl pipe -- runs out of tries and is reported.
        for (int attempt = 0; attempt < 3; ++attempt) {
            std::cout << "  " << question << (default_yes ? " [Y/n] " : " [y/N] ")
                      << std::flush;
            std::string answer;
            if (!std::getline(std::cin, answer)) {
                break;
            }
            if (answer.empty()) {
                return default_yes;
            }
            if (answer == "y" || answer == "Y" || answer == "yes" || answer == "Yes") {
                return true;
            }
            if (answer == "n" || answer == "N" || answer == "no" || answer == "No") {
                return false;
            }
            std::cout << "  please answer y or n.\n";
        }
        unanswerable = true;
        std::cout << "\n";
        return false;
    }
};

bool remove_path(const std::filesystem::path& target) {
    std::error_code ec;
    std::filesystem::remove_all(target, ec);
    if (ec) {
        std::cout << "  could not remove " << target.string() << ": " << ec.message() << "\n";
        if (ec == std::errc::permission_denied) {
            std::cout << "  try: sudo rm -rf " << target.string() << "\n";
        }
        return false;
    }
    return true;
}

/// $XDG_CACHE_HOME/crucible -- build trees left by install.sh when the checkout
/// was on a filesystem that cannot hold them. Pure cache, but it can be a
/// gigabyte, so it goes with the binary rather than being left behind.
std::filesystem::path cache_dir() {
    if (const char* value = std::getenv("XDG_CACHE_HOME"); value != nullptr && *value != '\0') {
        if (const std::filesystem::path candidate(value); candidate.is_absolute()) {
            return candidate / "crucible";
        }
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".cache" / "crucible";
    }
    return {};
}

#if defined(_WIN32)
constexpr std::string_view kExeSuffix = ".exe";
#else
constexpr std::string_view kExeSuffix = "";
#endif

/// The other programs this install put beside the binary.
///
/// `crucible` is not the whole install. `crucible-gui` is the desktop face of
/// the same engine and `crucible-routebench` is the developer tool; both are
/// placed in the same bin/ by the same install component, and both are useless
/// once the libraries under them are gone. Leaving either behind puts a
/// program on PATH that can only fail, and makes a clean reinstall test lie.
std::vector<std::filesystem::path> companion_programs(const std::filesystem::path& binary) {
    std::vector<std::filesystem::path> found;
    if (binary.empty()) {
        return found;
    }
    const std::filesystem::path dir = binary.parent_path();
    for (const std::string_view name : {"crucible-gui", "crucible-routebench"}) {
        std::filesystem::path candidate =
            dir / (std::string(name) + std::string(kExeSuffix));
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && candidate != binary) {
            found.push_back(std::move(candidate));
        }
    }
    return found;
}

/// The shared libraries that sit beside the executable rather than under
/// lib/crucible.
///
/// That is the Windows layout, and it is not a quirk of the installer: the
/// loader there looks next to the executable and not at an RPATH, so the
/// install rule puts them in bin/. Same files, different place, and an
/// uninstaller that only knows lib/crucible would leave every byte of
/// llama.cpp behind on Windows.
std::vector<std::filesystem::path> companion_libraries(const std::filesystem::path& binary) {
    std::vector<std::filesystem::path> found;
    if (binary.empty() || kExeSuffix.empty()) {
        return found;  // elsewhere they live in lib/crucible, handled below
    }
    const std::filesystem::path dir = binary.parent_path();
    for (const char* name : {"llama.dll", "ggml.dll", "ggml-base.dll"}) {
        std::filesystem::path candidate = dir / name;
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            found.push_back(std::move(candidate));
        }
    }
    return found;
}

/// The desktop entry and its icon.
///
/// Installed beside the binaries on Linux so crucible-gui appears in the
/// application menu, which means uninstall has to take them too. A .desktop
/// file left behind is worse than ordinary litter: the menu keeps offering
/// Crucible, and clicking it does nothing.
std::vector<std::filesystem::path> desktop_files(const std::filesystem::path& binary) {
    std::vector<std::filesystem::path> found;
    if (binary.empty()) {
        return found;
    }
    const std::filesystem::path share = binary.parent_path().parent_path() / "share";
    for (const std::filesystem::path& candidate :
         {share / "applications" / "crucible.desktop",
          share / "icons" / "hicolor" / "scalable" / "apps" / "crucible.svg"}) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            found.push_back(candidate);
        }
    }
    return found;
}

/// Tell the desktop the menu entry is gone.
///
/// Removing crucible.desktop is not the whole job on Linux: the entry is also
/// written into <share>/applications/mimeinfo.cache and the icon into
/// <share>/icons/hicolor/icon-theme.cache, and until those are rebuilt the
/// application menu goes on offering a Crucible that is not there. The caches
/// are shared with every other application in the prefix, so they are
/// regenerated rather than deleted.
///
/// Both tools are optional and neither failing is worth a word: the entry is
/// off disk either way, and every desktop rebuilds these on the next login.
/// The install runs the same pair for the same reason, in the other direction.
void refresh_desktop_caches(const std::filesystem::path& binary) {
#if !defined(_WIN32) && !defined(__APPLE__)
    if (binary.empty()) {
        return;
    }
    const std::filesystem::path share = binary.parent_path().parent_path() / "share";
    const std::vector<std::vector<std::string>> commands{
        {"update-desktop-database", (share / "applications").string()},
        {"gtk-update-icon-cache", "-qtf", (share / "icons" / "hicolor").string()},
    };
    for (const std::vector<std::string>& argv : commands) {
        if (!util::on_path(argv.front())) {
            continue;
        }
        util::Subprocess child;
        std::string      error;
        if (child.start(argv, {}, {}, error)) {
            std::string line;
            while (child.read_line(line)) {
            }
            child.wait();
        }
    }
#else
    (void)binary;
#endif
}

/// <prefix>/lib/crucible -- llama.cpp's shared libraries and the runtimes that
/// shipped with the install. The binary alone is not the whole program any
/// more, so removing it without these would leave most of the bytes behind.
std::filesystem::path library_dir(const std::filesystem::path& binary) {
    if (binary.empty()) {
        return {};
    }
    const std::filesystem::path candidate = binary.parent_path().parent_path() / "lib" / "crucible";
    return std::filesystem::exists(candidate) ? candidate : std::filesystem::path{};
}

}  // namespace

int run_uninstall(bool assume_yes) {
    Prompter prompt{assume_yes, false};
    const std::filesystem::path binary  = own_path();
    const std::filesystem::path config  = paths::config_dir();
    const std::filesystem::path data    = paths::data_dir();
    const std::filesystem::path models  = paths::models_dir();
    const std::filesystem::path libs    = library_dir(binary);
    const std::filesystem::path cache   = cache_dir();
    const std::vector<std::filesystem::path> programs = companion_programs(binary);
    const std::vector<std::filesystem::path> beside   = companion_libraries(binary);
    const std::vector<std::filesystem::path> desktop  = desktop_files(binary);

    // Everything to be removed, gathered before anything is said about it, so
    // the list on screen is exactly the list that will be acted on.
    std::vector<std::filesystem::path> targets;
    const auto take = [&targets](const std::filesystem::path& path) {
        std::error_code ec;
        if (!path.empty() && std::filesystem::exists(path, ec)) {
            targets.push_back(path);
        }
    };

    std::cout << "\n  Uninstalling Crucible\n\n";

    take(binary);
    if (!binary.empty() && std::filesystem::exists(binary)) {
        std::cout << "  program  " << binary.string() << "\n";
    }
    for (const std::filesystem::path& program : programs) {
        take(program);
        std::cout << "  program  " << program.string() << "\n";
    }
    for (const std::filesystem::path& lib : beside) {
        take(lib);
        std::cout << "  library  " << lib.string() << "\n";
    }
    if (!libs.empty()) {
        take(libs);
        std::cout << "  library  " << libs.string() << "  ("
                  << format::bytes(directory_size(libs)) << ")\n";
    }
    for (const std::filesystem::path& entry : desktop) {
        take(entry);
        std::cout << "  desktop  " << entry.string() << "\n";
    }
    if (std::filesystem::exists(config)) {
        take(config);
        std::cout << "  config   " << config.string() << "\n";
    }
    if (std::filesystem::exists(data)) {
        take(data);
        std::cout << "  data     " << data.string() << "  ("
                  << format::bytes(directory_size(data)) << ")\n";
    }
    if (!cache.empty() && std::filesystem::exists(cache)) {
        take(cache);
        std::cout << "  cache    " << cache.string() << "  ("
                  << format::bytes(directory_size(cache)) << ")\n";
    }

    if (targets.empty()) {
        std::cout << "  nothing found to remove.\n\n";
        return 0;
    }

    // The models are called out by name because they are the one thing here
    // Crucible did not put on disk -- they are files the user downloaded, they
    // are the largest thing in the list, and a reinstall does not bring them
    // back. Everything else is derived and comes back with the next install.
    const std::vector<ModelFile> found = scan_models(models);
    if (!found.empty()) {
        std::cout << "\n  The data directory holds " << found.size() << " model file"
                  << (found.size() == 1 ? "" : "s") << " ("
                  << format::bytes(directory_size(models)) << ") that you supplied.\n";
    }
    std::cout << "\n";

    if (!prompt.ask("Remove Crucible and everything above?", true)) {
        // A prompt that gave out is not the same as a refusal, and the caller
        // has to be able to tell them apart: install.sh falls back to its own
        // sweep for the first and leaves well alone for the second.
        if (prompt.unanswerable) {
            std::cout << "\n  There was no answer to read, so nothing was removed.\n"
                         "  Run this from a terminal, or pass -y to answer yes.\n\n";
            return 1;
        }
        std::cout << "\n  Nothing was removed.\n\n";
        return 0;
    }

    // Unlinking a running executable is fine on Linux and macOS: the kernel
    // keeps the inode alive until this process exits.
    bool removed_binary = false;
    for (const std::filesystem::path& target : targets) {
        if (remove_path(target)) {
            std::cout << "  removed " << target.string() << "\n";
            removed_binary = removed_binary || target == binary;
        }
    }
    if (!desktop.empty()) {
        refresh_desktop_caches(binary);
    }

    // --- what is actually left ---------------------------------------------
    //
    // The promise of this command is that it leaves nothing behind, and the
    // only honest way to make that claim is to look.
    std::vector<std::filesystem::path> left;
    for (const std::filesystem::path& target : targets) {
        std::error_code ec;
        if (std::filesystem::exists(target, ec)) {
            left.push_back(target);
        }
    }

    if (!left.empty()) {
        std::cout << "\n  These could not be removed:\n";
        for (const std::filesystem::path& path : left) {
            std::cout << "    " << path.string() << "\n";
        }
        std::cout << "\n  Remove them by hand, or re-run with sudo if they are"
                     " somewhere you cannot write.\n\n";
        return 1;
    }

    std::cout << "\n  Done.";
    if (!removed_binary && !binary.empty()) {
        std::cout << " The binary is still at " << binary.string() << ".";
    }
    std::cout << "\n\n";
    return 0;
}

}  // namespace crucible
