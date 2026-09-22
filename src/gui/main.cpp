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
#include "crucible/app/uninstall.hpp"
#include "crucible/config/config.hpp"
#include "crucible/config/paths.hpp"
#include "crucible/config/trust.hpp"
#include "crucible/session/store.hpp"
#include "crucible/util/platform.hpp"
#include "app.hpp"

namespace {

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

    // No project, and so no folder question yet.
    //
    // Crucible used to open on a directory it chose: the shell's working
    // directory from a terminal, the most recent project from the application
    // menu. Both are guesses, and the menu one guessed wrong every time the
    // launcher handed over `/` or the home directory -- a window would open on
    // somewhere nobody meant to work, asking to be trusted with it.
    //
    // It opens on nothing instead. The top bar says No Project, the one button
    // there is Open Project, and the folder question is asked when a folder is
    // actually chosen -- by the same modal that has always guarded it.
    std::vector<std::string> warnings;
    crucible::Config config = crucible::load_config(warnings);

    crucible::gui::App app(std::move(config), std::move(warnings),
                           options.skip_trust);
    return app.run();
}
