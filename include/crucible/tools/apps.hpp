// SPDX-License-Identifier: MIT
//
// Making a project's page into a program a person opens like any other.
//
// What Crucible builds for somebody who wants "a budgeting app" is a page --
// see the build loop's APP_GUIDE -- and the last step of asking for one is
// using it without Crucible: from Applications on a Mac, the Start menu on
// Windows, the application launcher on Linux. This is that step.
//
// The program is a copy of crucible-app (tools/app_runner.cpp) beside the
// page, bundled into one file with everything it references put inside it,
// and the app's name. Where it goes is where each platform keeps a person's
// own programs, needing no administrator: ~/Applications on a Mac,
// %LOCALAPPDATA%\Programs with a Start menu shortcut on Windows,
// ~/.local/share with a .desktop entry on Linux. Making it again replaces it,
// and the data the app saved -- kept in the person's data folder, not beside
// the program -- is left as it was.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace crucible::tools::apps {

/// What was made.
struct Made {
    std::string           name;      ///< what it is called
    std::filesystem::path program;   ///< the .app, or the folder holding the program
    std::filesystem::path launch;    ///< what is opened to run it
    std::string           where;     ///< where to find it, for a person
};

/// What a project's app is called: its page's <title>, or the folder's name.
std::string name_for(const std::filesystem::path& root, const std::string& page);

/// The runner: crucible-app beside the program that is running, or `""` when
/// it is not there -- a build of Crucible without a window has none.
std::filesystem::path runner();

/// Package `page` of the project at `root` as a program called `name`.
/// Nothing, with `error` saying why, when it could not be made.
///
/// With CRUCIBLE_APPS_DIR set, the program is made in a folder of that name
/// there and nothing is registered with the system -- what the tests use.
std::optional<Made> package(const std::filesystem::path& root, const std::string& page,
                            const std::string& name, std::string& error);

/// Open a program package made. False, with `error`, when it would not start.
bool open(const std::filesystem::path& launch, std::string& error);

namespace detail {

/// A name for files and identifiers: lower case, letters, digits and dashes.
std::string slug(const std::string& name);

/// A name a folder or a file can have on every platform.
std::string file_name(const std::string& name);

/// A Mac app's Info.plist.
std::string info_plist(const std::string& name, const std::string& identifier,
                       const std::string& executable);

/// A Linux .desktop entry.
std::string desktop_entry(const std::string& name, const std::filesystem::path& exec);

}  // namespace detail

}  // namespace crucible::tools::apps
