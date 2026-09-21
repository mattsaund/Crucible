// SPDX-License-Identifier: MIT
//
// Slash-command completion.
//
// Typing "/" opens a list of what could follow it, narrowing as more is typed,
// with the rest of the best match shown in gray after the cursor. Tab takes it.
//
// This file is the single list of commands Crucible has: /help prints it and the
// completion menu offers it, so a command cannot exist in one and not the
// other.
//
// It lives beside the router rather than under ui/ because there is no terminal
// anywhere in it -- matching a prefix against a list of names is a fact about
// strings. That is also what lets the unit tests reach it: they link the core
// library, which does not drag a window toolkit along.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "crucible/routing/expert.hpp"

namespace crucible::ui {

/// One entry in the menu.
struct CommandInfo {
    std::string name;     ///< without the slash: "resume"
    std::string summary;  ///< the line /help prints beside it
    /// True for the expert seats and for the commands that act on an argument,
    /// which take text after the command rather than acting on their own.
    bool takes_prompt = false;
};

/// Every command, in the order /help lists them: the built-ins first, then one
/// per expert seat on `roster`.
///
/// Built per call rather than cached, because the seats are no longer fixed:
/// `/newexpert` has to make its own command appear without a restart. The list
/// is two dozen short strings and is rebuilt only on a keystroke after a slash.
std::vector<CommandInfo> all_commands(const Roster& roster);

/// The commands `input` could still become.
///
/// Empty unless `input` is a slash followed by a partial word and nothing
/// else. "/re" matches; "/resume " does not, because the command is settled
/// and what follows is an argument; "hello" is not a command at all. A bare
/// "/" matches everything, which is what opens the menu.
std::vector<CommandInfo> command_matches(std::string_view input, const Roster& roster);

/// What Tab would add to `input` to complete it to `choice`, including the
/// trailing space for a command that takes a prompt. Empty when there is
/// nothing to add.
///
/// Returns only the *added* text, not the whole line, so the caller appends
/// rather than replaces -- which is also exactly what is drawn in gray.
std::string command_completion(std::string_view input, const CommandInfo& choice);

}  // namespace crucible::ui
