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
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

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

// --- updating in place ---------------------------------------------------------
//
// What a release ships for each platform, fetched and started by Crucible
// itself: the setup program on Windows, the disk image's application on a
// Mac, the AppImage on Linux. A copy built from source has no such file and
// keeps the one-line command above.

/// How this copy was installed, which decides what updates it.
enum class Install { Setup, AppBundle, AppImage, Source };

/// Read from where this program is: inside a .app bundle, an AppImage
/// ($APPIMAGE), Windows (whose installer installs over any prefix), or a
/// build from source.
Install install_here();

/// The release file that updates an install of this kind; empty for Source.
std::string asset_for(Install install);

/// Where `asset` downloads from, and its size, in GitHub's reply about a
/// release. False when the release does not carry it.
bool find_asset(std::string_view body, std::string_view asset, std::string& url, std::uint64_t& bytes);

/// The script that waits for this Crucible (`pid`) to close, puts `fetched`
/// in place of the install at `target`, and starts the new one. `target` is
/// the install folder on Windows, the .app on a Mac, the AppImage on Linux.
std::string swap_script(Install install, long pid, const std::filesystem::path& fetched,
                        const std::filesystem::path& target);

/// The newest release's installer, fetched and started on a thread of its
/// own. When it has been started, `launched` is called: the caller closes
/// Crucible, and the script finishes the job.
class Installer {
public:
    enum class Phase { Idle, Asking, Downloading, Starting, Failed };
    struct Progress {
        Phase         phase = Phase::Idle;
        std::uint64_t done  = 0;
        std::uint64_t total = 0;
        std::string   error;
        bool running() const { return phase == Phase::Asking || phase == Phase::Downloading
                                      || phase == Phase::Starting; }
    };

    Installer() = default;
    ~Installer();
    Installer(const Installer&)            = delete;
    Installer& operator=(const Installer&) = delete;

    /// False, with `error`, when one is already under way or nothing here
    /// can be updated this way.
    bool start(std::function<void()> launched, std::string& error);
    Progress progress() const;

private:
    void run(std::function<void()> launched);
    void set(Phase phase, std::string error = {});

    mutable std::mutex mutex_;
    Progress           progress_;
    std::filesystem::path downloading_;
    std::thread        worker_;
};

}  // namespace crucible::update
