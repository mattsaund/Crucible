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
#include "crucible/app/relocate.hpp"
#include "crucible/app/setup_runtimes.hpp"
#include "crucible/app/setup_trainer.hpp"
#include "crucible/app/uninstall.hpp"
#include "crucible/config/config.hpp"
#include "crucible/config/paths.hpp"
#include "crucible/config/trust.hpp"
#include "crucible/kit/kit.hpp"
#include "crucible/session/store.hpp"
#include "crucible/util/platform.hpp"
#include "app.hpp"
#include "crash.hpp"

int main(int argc, char** argv) {
    // Started from a terminal, `crucible --help` prints the braille mark, and
    // on Windows the console needs telling before it can render it.
    crucible::util::use_utf8_console();

    // The programs Crucible fetched for its experts, ahead of the machine's
    // own on this process's PATH, so every command an expert runs finds them.
    // See kit/kit.hpp.
    crucible::kit::put_on_path();

    const crucible::app::Options options = crucible::app::parse_arguments(argc, argv);
    if (options.should_exit) {
        return options.exit_code;
    }

    if (options.uninstall) {
        return crucible::run_uninstall(options.assume_yes);
    }

    // Both of these are the installer talking to the program it just built,
    // and both finish without opening a window.
    if (options.runtime_status) {
        return crucible::run_runtime_status();
    }
    if (options.install_python) {
        return crucible::run_python_setup(options.quiet, options.force);
    }
    if (options.install_kit) {
        return crucible::run_kit_setup(options.quiet);
    }
    if (options.install_runtimes) {
        return crucible::run_runtime_setup(options.quiet, options.force);
    }
    if (options.trainer_status) {
        return crucible::run_trainer_status();
    }
    if (options.install_trainer) {
        return crucible::run_trainer_setup(options.trainer_flavor, options.force,
                                           options.quiet);
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
    // From here on, a crash says where it happened. See crash.hpp.
    crucible::gui::crash::install(crucible::paths::data_dir() / "crash.log");

    // The Scratchpad used to be ~/Crucible, in plain sight; it is in a hidden
    // folder now, and a Crucible from before left its chats in the old one.
    // Moved before anything here has read a path that names it.
    std::vector<std::string> warnings = crucible::relocate::scratchpad();
    crucible::Config config = crucible::load_config(warnings);

    crucible::gui::App app(std::move(config), std::move(warnings),
                           options.skip_trust);
    return app.run();
}
