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
    std::string   version;   ///< "3.14.9"
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

/// Whether the virtual environment at `venv` was made from Crucible's Python
/// at another minor version than the one pinned now. A venv belongs to the
/// interpreter it was made from: once Crucible's Python moves from 3.12 to
/// 3.14, a venv made from 3.12 still starts, and imports nothing it installed.
/// False for a venv made from any other Python, which is not Crucible's to
/// judge, and when CRUCIBLE_PYTHON names the interpreter.
bool made_from_another_python(const std::filesystem::path& venv);

/// Where a download or an unpack has got to.
struct Progress {
    std::string   phase;       ///< "downloading", "checking", "unpacking"
    std::uint64_t done  = 0;   ///< bytes so far, while downloading
    std::uint64_t total = 0;
};

/// Fetch, check and unpack it, replacing whatever is in root(). Blocks; call
/// it from a thread that may wait. `on_progress` is called from that thread.
///
/// `stop` is asked a few times a second, and true ends it with "stopped".
/// root() is never left half written: what was there stays until a whole,
/// checked download has been unpacked beside it. What was downloaded is kept
/// for the next install to carry on from.
bool install(const std::function<void(const Progress&)>& on_progress, std::string& error,
             const std::function<bool()>& stop = {});

/// True while an install() is running, anywhere in this process -- so a
/// prompt sent in the first minute of a new install can wait for the Python
/// it needs rather than fail for want of it.
bool installing();

// --- exposed for the tests -------------------------------------------------

namespace detail {

/// install(), for `build` rather than the one this platform pins: the tests
/// fetch a small archive of their own from a file:// address with it.
bool install(const Build& build, const std::function<void(const Progress&)>& on_progress,
             std::string& error, const std::function<bool()>& stop);

/// Where `build` is downloaded to, by way of a ".part" beside it, until it
/// has been unpacked.
std::filesystem::path download_file(const Build& build);

}  // namespace detail

}  // namespace crucible::lab::python
