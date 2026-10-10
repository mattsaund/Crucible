// SPDX-License-Identifier: MIT
//
// Moving the Scratchpad out of sight.
//
// The Scratchpad -- where a conversation with no project open keeps what it
// makes, a folder for each -- was ~/Crucible/Scratchpad, in plain sight in
// every file browser and every listing of a home folder. It is in
// ~/.crucible now (see paths::scratchpad_dir), hidden the way each system
// hides a folder.
//
// A machine that ran a Crucible from before has the old one, and it is named,
// by path, in more places than the folder itself: each chat's record of the
// folder it was had in, each build's journal, the folder that holds a
// project's history -- named for a hash of the project's path -- the list of
// recent projects, and the trust given to the Scratchpad. Moving the folders
// and leaving those would lose every chat that was in them, so this moves the
// lot.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace crucible::relocate {

/// Move the Scratchpad from where it was to where it is, if there is an old
/// one, and point everything that named a folder in it at where that folder
/// is now. Returns what is worth telling the person -- what was moved, or what
/// could not be -- and nothing when there was nothing to do, which is every
/// start but the first. Run before the configuration, the trust or any
/// history is read, so that nothing has read the old paths yet.
std::vector<std::string> scratchpad();

/// The same between two folders named outright: `from` is moved into `to`,
/// merging with what is already there. For the tests, and for scratchpad().
std::vector<std::string> scratchpad(const std::filesystem::path& from, const std::filesystem::path& to);

}  // namespace crucible::relocate
