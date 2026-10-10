// SPDX-License-Identifier: MIT
//
// The kit: the programs Crucible fetches for its experts, so nobody has to.
//
// An expert building software reaches for programs -- GitHub's command line
// to publish, Node to run a JavaScript project, a browser to read a page that
// is drawn by script, git itself on a Windows machine that has none, and on
// Linux something to read the words off a picture. A person who installed
// Crucible to have a program made for them should not be sent to install any
// of these by hand, so Crucible fetches what this machine lacks, when it
// starts (see app/setup.hpp), from each program's own published downloads.
//
// Each piece is a folder under data_dir()/kit, and the folders holding their
// programs are put at the front of Crucible's own PATH -- so `RUN: node
// test.js`, `GH: repo create` and every check for whether a program is there
// find them, and nothing outside Crucible is changed. What the machine has
// already is used as it is and never fetched again: a kit piece is only for
// what is missing.
//
// Like a runtime or Crucible's Python, nothing here is shipped with
// Crucible; each is downloaded from where its makers publish it. See
// THIRD_PARTY.md for what each is and its license.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace crucible::kit {

/// One program the kit can fetch.
struct Piece {
    std::string   id;      ///< "gh", "node", "browser", "git", "ocr"
    std::string   label;   ///< "GitHub's command line"
    std::string   why;     ///< "publishing to GitHub, and releases"
    std::uint64_t bytes = 0;   ///< about what it downloads
};

/// Every piece the kit knows, whether or not this machine wants it.
std::vector<Piece> pieces();

/// Where the kit lives: data_dir()/kit, a folder per piece.
std::filesystem::path dir();

/// Whether `id` has been fetched into the kit.
bool installed(const std::string& id);

/// The pieces this machine lacks, which start() of the setup fetches: each
/// one whose program is neither on the machine nor in the kit already.
std::vector<Piece> missing();

/// The folders holding the kit's programs, the ones to put on PATH.
std::vector<std::filesystem::path> bin_dirs();

/// Put the kit's programs at the front of this process's PATH, so every
/// command an expert runs and every check for a program finds them. Called
/// as Crucible starts and after each install; harmless to call again.
void put_on_path();

/// Fetch and unpack one piece. `progress` is told (done, total) bytes as the
/// download arrives; `cancel` is asked between steps. False, with `error`
/// saying why, when it could not be had.
bool install(const std::string& id, const std::function<void(std::uint64_t, std::uint64_t)>& progress,
             const std::function<bool()>& cancel, std::string& error);

/// The kit's headless browser, when it has one: Chrome's headless shell.
std::filesystem::path browser();

/// The kit's Python for reading text off pictures, when it has one: Linux
/// without tesseract.
std::filesystem::path ocr_python();

namespace detail {

/// What this machine is, as the downloads name it.
struct Platform {
    std::string os;     ///< "macos", "windows", "linux"
    std::string arch;   ///< "arm64", "x64"
};
Platform platform_here();

/// The GitHub CLI's asset for `here`, from a release listing: its URL, and
/// its size in `bytes`. Empty when there is none.
std::string gh_asset(std::string_view release_json, const Platform& here, std::uint64_t& bytes);

/// Node's current long-term-support download for `here`, from nodejs.org's
/// index: the URL, with the version in `version`. Empty when there is none.
std::string node_url(std::string_view index_json, const Platform& here, std::string& version);

/// Chrome's headless shell for `here`, from Chrome for Testing's listing.
std::string browser_url(std::string_view listing_json, const Platform& here);

/// MinGit for `here`, from Git for Windows' release listing, and its size.
std::string mingit_asset(std::string_view release_json, const Platform& here, std::uint64_t& bytes);

}  // namespace detail

}  // namespace crucible::kit
