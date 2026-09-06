// SPDX-License-Identifier: MIT
#include "crucible/app/cli.hpp"

#include <iostream>

#include "crucible/config/paths.hpp"

namespace crucible::app {
namespace {

// Crucible's mark, in braille, from packaging/flame.txt -- the one copy the
// two installers, the README and this are all pasted from. The terminal
// sprite draws the same flame in plain ASCII instead, because it has to
// animate and it has a no-Unicode fallback; this is a still, and can afford
// the resolution braille gives it.
//
// Printed as UTF-8. main() puts the console into UTF-8 first, which on
// Windows is the difference between this and twelve rows of question marks.
constexpr const char* kBanner = R"(
      ⠀⠀⠀⠀⠀⠀⢱⣆⠀⠀⠀⠀⠀⠀
      ⠀⠀⠀⠀⠀⠀⠈⣿⣷⡀⠀⠀⠀⠀
      ⠀⠀⠀⠀⠀⠀⢸⣿⣿⣷⣧⠀⠀⠀
      ⠀⠀⠀⠀⡀⢠⣿⡟⣿⣿⣿⡇⠀⠀
      ⠀⠀⠀⠀⣳⣼⣿⡏⢸⣿⣿⣿⢀⠀   Crucible )" CRUCIBLE_VERSION R"(
      ⠀⠀⠀⣰⣿⣿⡿⠁⢸⣿⣿⡟⣼⡆   a local LLM engine that delegates.
      ⢰⢀⣾⣿⣿⠟⠀⠀⣾⢿⣿⣿⣿⣿
      ⢸⣿⣿⣿⡏⠀⠀⠀⠃⠸⣿⣿⣿⡿
      ⢳⣿⣿⣿⠀⠀⠀⠀⠀⠀⢹⣿⡿⡁
      ⠀⠹⣿⣿⡄⠀⠀⠀⠀⠀⢠⣿⡞⠁
      ⠀⠀⠈⠛⢿⣄⠀⠀⠀⣠⠞⠋⠀⠀
      ⠀⠀⠀⠀⠀⠀⠉⠀⠀⠀⠀⠀⠀⠀
)";

void print_usage() {
    std::cout << kBanner << R"(
usage: crucible [options]

  -h, --help       show this and exit
  -v, --version    print the version and exit
      --config     print the config file path and exit
      --uninstall  remove Crucible, its config, and its data
      --no-trust   skip the folder trust prompt for this run
  -y, --yes        with --uninstall, answer yes to everything

Crucible reads its configuration from:
)" << "  " << paths::config_file().string() << "\n\n";
}

}  // namespace

const char* banner() {
    return kBanner;
}

Options parse_arguments(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];

        if (argument == "-h" || argument == "--help") {
            print_usage();
            options.should_exit = true;
            return options;
        }
        if (argument == "-v" || argument == "--version") {
            std::cout << "crucible " CRUCIBLE_VERSION "\n";
            options.should_exit = true;
            return options;
        }
        if (argument == "--config") {
            std::cout << paths::config_file().string() << "\n";
            options.should_exit = true;
            return options;
        }
        if (argument == "--no-trust") { options.skip_trust = true;  continue; }
        if (argument == "--uninstall") { options.uninstall = true;  continue; }
        if (argument == "-y" || argument == "--yes") { options.assume_yes = true; continue; }

        std::cerr << "crucible: unknown option '" << argument << "' (try --help)\n";
        options.should_exit = true;
        options.exit_code   = 2;
        return options;
    }

    return options;
}

}  // namespace crucible::app
