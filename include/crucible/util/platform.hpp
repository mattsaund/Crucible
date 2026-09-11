// SPDX-License-Identifier: MIT
//
// The handful of things that are genuinely different per operating system.
//
// Crucible is otherwise portable C++: the standard library covers filesystem,
// threads and time, and llama.cpp and FTXUI cover the rest. What is left is
// three questions the standard has no answer to -- where am I, what time is it
// locally, and how do I run a shell command -- and they are gathered here so
// the rest of the program can be written once.
//
// Every function has a working implementation on Linux, macOS and Windows, and
// a defined answer when it cannot find one. `executable_path` returning empty,
// for instance, is a normal outcome the uninstaller reports rather than a case
// that cannot arise.
#pragma once

#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace crucible::util {

/// The path of the running executable, or empty when it cannot be determined.
///
/// Three different calls for three platforms, and the reason it is worth a
/// function of its own: `/proc/self/exe` is Linux, and reading it on macOS
/// silently produced an empty path -- which made `crucible --uninstall` unable
/// to find the binary that was asking the question.
std::filesystem::path executable_path();

/// The local time for `when`.
///
/// `localtime_r` is POSIX and `localtime_s` is Windows, with the arguments the
/// other way round. Neither is `std::localtime`, which returns a pointer to a
/// shared buffer and is a data race waiting for a second thread.
std::tm local_time(std::time_t when);

/// The UTC time for `when`.
///
/// The same split as local_time: `gmtime_r` on POSIX, `gmtime_s` on Windows
/// with the arguments reversed. The runtime builder used to call `gmtime_r`
/// itself, and no Windows compiler has it.
std::tm utc_time(std::time_t when);

/// Is there a terminal on standard input to ask a question on?
///
/// The difference between `crucible-gui` started from a shell and the same
/// binary started from the application menu. Launched from a menu there is no
/// terminal at all: a prompt written to stdout goes nowhere, the read of the
/// answer fails at once, and a program that treats that as "no" exits without
/// ever opening a window -- which is exactly what clicking the icon used to do.
bool stdin_is_a_terminal();

/// Put this program's console output into UTF-8, where that is a thing.
///
/// A no-op everywhere but Windows, whose console starts on a legacy code page
/// and renders every byte of a multi-byte character as its own question mark.
/// Crucible's banner is drawn in braille and its terminal face in box-drawing
/// characters, so without this the first thing a Windows user sees is mojibake.
/// Called once, at the top of main, before anything is printed.
void use_utf8_console();

/// The argv that runs `command` through the platform's shell.
///
/// A shell rather than an argv split, because pipes, redirections and `&&` are
/// how anyone describes running a project and splitting the string would break
/// all three. See tools/workshop.hpp for what that does and does not confine.
std::vector<std::string> shell_command(const std::string& command);

}  // namespace crucible::util
