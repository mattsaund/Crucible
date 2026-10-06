// SPDX-License-Identifier: MIT
//
// Command-line parsing, and the banner everything else prints above it.
#pragma once

#include <string>

namespace crucible::app {

/// What the command line asked for.
struct Options {
    bool skip_trust = false;  ///< --no-trust: do not ask about this folder
    bool uninstall  = false;  ///< --uninstall
    bool assume_yes = false;  ///< -y, only meaningful with --uninstall

    /// --install-trainer: put the Python training environment in place and
    /// exit. What the installer runs once the program is built, and what
    /// repairs the environment afterwards without a reinstall.
    bool install_trainer = false;
    bool trainer_status  = false;  ///< --trainer-status

    /// --install-runtimes: put the compute backends this machine can use in
    /// place. What the installer runs so a fresh install can load a model
    /// without a visit to the settings screen.
    bool install_runtimes = false;
    bool runtime_status   = false;  ///< --runtime-status

    /// --install-python: fetch Crucible's own Python and exit. Routing and
    /// cooks run on it; the window fetches it on first start otherwise.
    bool install_python = false;
    bool force           = false;  ///< --force, with --install-trainer
    bool quiet           = false;  ///< --quiet, for an installer's own output

    /// --trainer-flavor cuda|cpu|mlx. Empty means whatever the hardware says.
    std::string trainer_flavor;


    /// Set when the program should stop after parsing -- --help, --version and
    /// --config all print something and exit, as does a bad option.
    bool should_exit = false;
    int  exit_code   = 0;
};

/// The flame shown by --help and the trust prompt.
const char* banner();

/// Parse `argv`, printing usage or version as required. Never throws; an
/// unknown option sets `should_exit` with a non-zero `exit_code`.
Options parse_arguments(int argc, char** argv);

}  // namespace crucible::app
