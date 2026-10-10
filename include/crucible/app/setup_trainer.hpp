// SPDX-License-Identifier: MIT
//
// Putting the training environment in place from a command line.
//
// The same code the settings screen drives, with a progress line instead of a
// progress bar. It exists so the installer can do this: `install.sh` builds
// the program and then asks the program it just built to finish setting
// itself up, rather than carrying a second copy of the venv-and-pip logic in
// shell -- and a third in PowerShell.
//
// It is also the repair command. A machine that had no Python at install
// time, or that lost the environment to a disk clean, is fixed with
// `crucible --install-trainer` and no reinstall.
#pragma once

#include <string>

namespace crucible {

/// Install the training environment, printing what it is doing.
///
/// `flavor` is "cuda", "cpu", "mlx" or empty for whatever the hardware says.
/// Returns a process exit code: 0 installed, 1 failed, 2 nothing to do
/// because it is already there and `force` was not given.
int run_trainer_setup(const std::string& flavor, bool force, bool quiet);

/// `crucible --install-python`: fetch Crucible's own Python, the one routing,
/// cooks and the training environment run on. 0 when it is in place, whether
/// it was just fetched or already there; 1 when it could not be had.
int run_python_setup(bool quiet, bool force);

/// Print what is installed. Returns 0 when the trainer could run right now.
int run_trainer_status();

/// `crucible --install-kit`: fetch every kit piece this machine lacks. 0 when
/// nothing is missing any more, 1 when a piece could not be had.
int run_kit_setup(bool quiet);

}  // namespace crucible
