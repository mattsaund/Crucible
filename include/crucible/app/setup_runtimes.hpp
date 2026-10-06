// SPDX-License-Identifier: MIT
//
// Putting the compute runtimes in place from a command line.
//
// Crucible used to install none of these and say so: which backend to build is
// a question only the machine can answer, and it was asked from the settings
// screen the first time somebody wanted a model to run on their card. That was
// the right shape when a runtime meant a multi-gigabyte toolkit and several
// minutes of nvcc.
//
// It is not the shape any more. The releases carry prebuilt modules for every
// platform, so the answer to "which backend does this machine want" can be
// acted on in seconds and a few tens of megabytes -- and a program that
// installs itself and then cannot run a model until you find a settings page
// is a program that does not work out of the box. So the installer asks the
// question and acts on it, and the settings screen stays for changing the
// answer.
#pragma once

#include <vector>

#include "crucible/runtime/backend.hpp"

namespace crucible {

/// Install the runtimes this machine wants: the processor backend always, and
/// whichever GPU backends the hardware can actually use.
///
/// Prefers the prebuilt module published for this platform and this llama.cpp
/// tag. Falls back to compiling only where a toolchain is already present --
/// an installer is the wrong place to start a four-minute nvcc run, so a
/// backend that would need one is reported and skipped rather than attempted.
///
/// Returns a process exit code: 0 when every runtime it wanted is in place,
/// 1 when at least one could not be, and 2 when there was nothing to do.
int run_runtime_setup(bool quiet, bool force);

/// The backends this machine should end up with: the CPU always, CUDA where
/// there is an NVIDIA driver, and Metal or Vulkan where the platform has them.
std::vector<BackendKind> runtimes_wanted_here();

/// Print what is installed, what this machine could use, and what is missing.
/// Returns 0 when at least one runtime is active.
int run_runtime_status();

}  // namespace crucible
