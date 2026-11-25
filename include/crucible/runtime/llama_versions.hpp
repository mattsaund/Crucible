// SPDX-License-Identifier: MIT
//
// Which llama.cpp the program runs on, and changing it without a new Crucible.
//
// Crucible is a runtime: the program as a module, built with one llama.cpp,
// which the launcher in bin/ loads (see src/launcher/launcher.cpp). The
// installer brings one. A newer llama.cpp, for a model the one here cannot
// run, is another runtime of the same Crucible, published as a release asset
// of its own with the compute backends built with it:
//
//   runtime-core-<platform>-<crucible version>-<llama tag>.tar.gz
//       the program built with that llama.cpp, and llama.cpp's libraries,
//       kept in runtimes/core/<crucible version>/<llama tag>/
//   runtime-<backend>-<platform>-<llama tag>.tar.gz
//       its backends, kept in runtimes/backends/<llama tag>/
//
// runtimes/active names the one chosen; the next start loads it. As in LM
// Studio: one at a time, updated and changed without updating the program.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace crucible::llama_versions {

/// One llama.cpp a runtime of this Crucible can be built with.
struct Version {
    std::string   tag;               ///< "b12701"
    bool          running   = false; ///< the runtime this program is
    bool          bundled   = false; ///< the one the installer put beside the program
    bool          chosen    = false; ///< the next start loads it
    bool          installed = false; ///< its runtime is on disk
    std::uint64_t bytes     = 0;     ///< its program's download, when published
    std::string   url;               ///< where that is, when published
    std::uint64_t disk      = 0;     ///< what it takes here: its program and backends

    /// The backends in use now that this one has none of ("CUDA"): one not
    /// published for it, which running on it would go without. Only for one
    /// installed and not running.
    std::vector<std::string> lacking;
};

/// The llama.cpp this runtime was built with.
std::string running_tag();

/// The one runtimes/active names for the next start, or empty for the
/// runtime the installer put beside the program.
std::string chosen_tag();

/// "b" and digits: what llama.cpp tags its builds.
bool is_tag(std::string_view tag);

/// "b12701" as 12701, for ordering. -1 for anything else.
long long tag_number(std::string_view tag);

/// Where this Crucible's runtime built with `tag` is kept.
std::filesystem::path core_dir(std::string_view tag);

/// The release asset that runtime is published as.
std::string core_asset(std::string_view tag);

/// Every version on disk, and every one `releases_json` (GitHub's release
/// list) has a runtime of this Crucible in, newest first. Backends left by a
/// llama.cpp nothing here runs any more are listed too, to be deleted.
std::vector<Version> list(std::string_view releases_json);

/// Fetch `tag`'s runtime and its backends (each one the running runtime has)
/// from `releases_json`, ready to choose. `say` hears each step.
bool install(std::string_view tag, std::string_view releases_json, std::string& error,
             const std::function<void(std::string)>& say = {});

/// The next start loads `tag`'s runtime; empty goes back to the one the
/// installer brought.
bool choose(std::string_view tag, std::string& error);

/// Delete `tag`'s runtime and backends. Not the running one, nor the chosen.
bool remove(std::string_view tag, std::string& error);

// --- in the background, one at a time ------------------------------------

/// GitHub's release list as last read, and when (seconds), shared by the
/// page and the daily check. Read again with refresh_releases.
std::string releases();
std::int64_t checked_at();
std::string check_error();
bool refresh_releases(std::string& error);

/// A fetch under way or just finished.
struct Fetching {
    std::string tag;     ///< being fetched, or empty
    std::string said;    ///< its last step
    std::string error;   ///< why the last one stopped, when it failed
    std::string ready;   ///< the last one fetched, ready to choose
    bool automatic = false;   ///< fetched by the daily update, and chosen
};
Fetching fetching();

/// Fetch `tag` on a thread of its own, from releases(). False when one is
/// already being fetched. `automatic`, the daily update's: chosen for the
/// next start once it is in, unless it lacks a backend in use now.
bool start_fetch(std::string tag, bool automatic = false);

/// The newest published version newer than both the running and the chosen
/// one, or empty: what an update would fetch.
std::string newer_than_here();

}  // namespace crucible::llama_versions
