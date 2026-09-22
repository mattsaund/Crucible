// SPDX-License-Identifier: MIT
//
// Knowing there is a newer Crucible.
//
// Crucible installs by compiling, which means a copy of it is a snapshot of
// whatever main looked like that afternoon, and nothing on the machine ever
// says otherwise. A fix for a crash on one platform is no use to the person
// who hit the crash if they never learn it exists.
//
// So: one HTTP GET to GitHub's releases API, at most once a day, asking a
// public endpoint for a version string. Nothing about the machine, the models,
// the config or anything typed goes with it -- it is the same request a browser
// makes opening the releases page, and the answer is a number to compare.
//
// It can be turned off (`ui.check_updates`), the answer is cached so the check
// is not made again for a day, and a failed check is silent: an update notice
// is a courtesy, and a program that complains about the network while you are
// trying to work is not.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace crucible::update {

/// The version this binary was compiled as.
std::string_view current();

/// Compare two version strings: -1 when a is older, 0 when they match, 1 when a
/// is newer. A leading `v` is ignored, missing parts count as zero (so "0.5"
/// and "0.5.0" are the same version), and anything after a dash is a pre-release
/// suffix, which ranks below the same version without one.
int compare(std::string_view a, std::string_view b);

/// The URL asked for the newest release.
std::string releases_url();

/// Pull the version and the release page out of GitHub's reply.
///
/// Reads `tag_name` and `html_url`, and returns false for anything that is not
/// an object with a tag in it -- a rate-limit message, an error, an empty repo
/// with no releases yet. A reply that cannot be read is not an update.
bool parse_latest(std::string_view body, std::string& version, std::string& page);

/// What is remembered between runs, in <data>/update.json.
struct State {
    std::string  latest;           ///< the newest version seen, empty when unknown
    std::string  page;             ///< where a person reads about it
    std::int64_t checked_at = 0;   ///< unix seconds of the last completed check
};

std::filesystem::path cache_file();
bool read_cache(State& state);
bool write_cache(const State& state);

/// Seconds between checks. A day: releases are not that frequent, and the
/// point is to be told within a day or so, not within a minute.
constexpr std::int64_t kInterval = 24 * 60 * 60;

/// Whether the cached answer is old enough to ask again.
bool due(const State& state, std::int64_t now, std::int64_t every = kInterval);

/// True when `state.latest` is a version newer than this build.
bool newer_than_this(const State& state);

/// The one line that updates an install on this platform.
std::string_view update_command();

/// Ask GitHub, and write the reply into `body`. Blocking -- the caller runs it
/// off whatever thread is drawing.
bool fetch(std::string& body, std::string& error);

/// The whole thing: read the cache, ask if it is due, write the cache back.
/// Returns the state it ended with, asked for or cached.
State refresh(bool allowed_to_ask);

}  // namespace crucible::update
