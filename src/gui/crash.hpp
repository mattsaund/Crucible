// SPDX-License-Identifier: MIT
//
// What the program was doing when it died, written down before it goes.
//
// A crash in the window is a window that vanishes: no dialog, and on Windows
// not even a line on a terminal nobody has open. Somebody reporting it can say
// "it crashed when I sent a prompt" and nothing more -- which is how a crash
// that only happened on Windows went unfound. So the program records its own:
// the fault, the calls that led to it, and for an exception nobody caught,
// what it said. Appended to data_dir()/crash.log, and pointed at on the next
// start.
#pragma once

#include <filesystem>
#include <string>

namespace crucible::gui::crash {

/// Start recording crashes into `log`. Once, at the top of main.
void install(const std::filesystem::path& log);

/// The line to show on this start when the last run ended in a crash it
/// recorded, or empty. Each crash is pointed at once.
std::string since_last_start(const std::filesystem::path& log);

}  // namespace crucible::gui::crash
