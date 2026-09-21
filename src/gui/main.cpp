// SPDX-License-Identifier: MIT
//
// crucible -- the entry point.
//
// Deliberately thin: parse the command line, deal with the folder trust gate,
// then hand off. Everything interesting is somewhere else, and nearly all of it
// is in crucible::core rather than in the window.

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "crucible/app/cli.hpp"
#include "crucible/app/trust_gate.hpp"
#include "crucible/app/uninstall.hpp"
#include "crucible/config/config.hpp"
#include "crucible/config/paths.hpp"
#include "crucible/config/trust.hpp"
#include "crucible/session/store.hpp"
#include "crucible/util/platform.hpp"
#include "app.hpp"

namespace {

/// Where a windowed Crucible should open when nobody said.
///
/// Started from a shell, the answer is the shell's directory: you cd somewhere
/// and run it, exactly as the terminal program works. Started from the
/// application menu there is no such intent -- the launcher hands over whatever
/// working directory it happens to have, usually the home directory or `/` --
/// and opening a session on that is not what anyone meant by clicking the icon.
///
/// So a menu launch reopens the last project instead, which is the thing a
/// desktop application is expected to do. If there is no last project, or it
/// has been moved or deleted, it falls back to the working directory and the
/// window asks about it.
std::filesystem::path where_to_open(bool from_a_terminal) {
    std::error_code ec;
    const std::filesystem::path working = std::filesystem::current_path(ec);
    if (from_a_terminal) {
        return ec ? std::filesystem::path(".") : working;
    }

    for (const crucible::Project& recent : crucible::recent_projects(8)) {
        if (std::filesystem::is_directory(recent.root, ec)) {
            return recent.root;
        }
    }
    return ec ? std::filesystem::path(".") : working;
}

}  // namespace

int main(int argc, char** argv) {
    // Started from a terminal, `crucible --help` prints the braille mark, and
    // on Windows the console needs telling before it can render it.
    crucible::util::use_utf8_console();

    const crucible::app::Options options = crucible::app::parse_arguments(argc, argv);
    if (options.should_exit) {
        return options.exit_code;
    }

    if (options.uninstall) {
        return crucible::run_uninstall(options.assume_yes);
    }

    // Trust is a decision about a directory, and it is the same decision in both
    // faces -- answering it in one answers it in the other. Where it gets asked
    // is the only thing that differs, and it has to follow the terminal.
    //
    // It used to be asked on stdin unconditionally. From the application menu
    // there is no stdin: the question went nowhere, the read of the answer
    // failed immediately, that was taken for "no", and the process exited
    // before opening a window. Clicking the icon did nothing at all, twice, and
    // then people stopped clicking it.
    //
    // With no terminal the window asks instead. It already knows how -- it is
    // the same modal that guards opening a project from the sidebar.
    const bool from_a_terminal = crucible::util::stdin_is_a_terminal();
    const std::filesystem::path start = where_to_open(from_a_terminal);

    bool ask_in_the_window = false;
    if (!options.skip_trust) {
        if (from_a_terminal) {
            if (!crucible::app::ensure_trusted(start)) {
                return 1;
            }
        } else {
            const crucible::TrustStore trust(crucible::paths::trust_file());
            ask_in_the_window = !trust.is_trusted(start);
        }
    }

    std::vector<std::string> warnings;
    crucible::Config config = crucible::load_config(warnings);

    crucible::gui::App app(std::move(config), std::move(warnings), start,
                           ask_in_the_window);
    return app.run();
}
