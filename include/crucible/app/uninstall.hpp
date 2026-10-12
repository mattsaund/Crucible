// SPDX-License-Identifier: MIT
// `crucible --uninstall`: remove Crucible without hunting for what it left behind.
#pragma once

#include <filesystem>

namespace crucible {

/// Remove the running binary, the configuration, and the data directory.
///
/// Each is a separate question so a partial uninstall is possible, but
/// answering yes to all of them leaves nothing behind -- which is what a clean
/// reinstall test needs. `assume_yes` answers yes to every question.
///
/// Returns a process exit code.
int run_uninstall(bool assume_yes);

/// Whether the Mac application at `app` is this install's: the launcher
/// install.sh writes, which execs `binary`, or the disk image's application
/// `binary` runs inside. Uninstalling takes no other -- not another install's,
/// and not the real one when the suite uninstalls a copy in a sandbox, which
/// is what every run of that test used to do to /Applications/Crucible.app.
bool app_belongs_to(const std::filesystem::path& app, const std::filesystem::path& binary);

}  // namespace crucible
