// SPDX-License-Identifier: MIT
//
// Crucible's own Python.
//
// Routing and the cook loop are Python (see orchestra/link.hpp), and so are the
// trainer and MLX's server. None of that may depend on whatever Python a
// machine happens to have -- a Mac has none worth the name, Windows has none at
// all, and a Linux distribution's is the system's, to be left alone. So
// Crucible fetches one of its own: a standalone CPython build from the
// python-build-standalone project, the same interpreter on every platform,
// unpacked into Crucible's data folder and removed by deleting that folder.
//
// Pinned, by version and by SHA-256. A newer build is a deliberate change made
// here, and a download that does not hash to the digest compiled in beside its
// address is deleted rather than run.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace crucible::lab::python {

/// The build of CPython this platform gets.
struct Build {
    std::string   version;   ///< "3.12.15"
    std::string   url;
    std::uint64_t bytes = 0; ///< the archive's size, which is how a download measures itself
    std::string   sha256;
};

/// Nothing for a platform no build is published for.
std::optional<Build> build_here();

std::filesystem::path root();         ///< data_dir()/python
std::filesystem::path marker_file();  ///< root()/crucible-python.json, written last

/// The interpreter. CRUCIBLE_PYTHON names another one instead -- for the tests
/// and CI, which have a Python of their own and no reason to download this.
std::filesystem::path interpreter();

/// Whether the interpreter is there, and is the build this Crucible pins.
bool installed();

/// The version installed, or empty.
std::string installed_version();

/// Where a download or an unpack has got to.
struct Progress {
    std::string   phase;       ///< "downloading", "checking", "unpacking"
    std::uint64_t done  = 0;   ///< bytes so far, while downloading
    std::uint64_t total = 0;
};

/// Fetch, check and unpack it, replacing whatever is in root(). Blocks; call
/// it from a thread that may wait. `on_progress` is called from that thread.
bool install(const std::function<void(const Progress&)>& on_progress, std::string& error);

/// True while an install() is running, anywhere in this process -- so a
/// prompt sent in the first minute of a new install can wait for the Python
/// it needs rather than fail for want of it.
bool installing();

}  // namespace crucible::lab::python
