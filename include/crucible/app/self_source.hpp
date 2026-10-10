// SPDX-License-Identifier: MIT
//
// Crucible, working on itself.
//
// A build is a directive and a project, and Crucible's own source is a
// project like any other: open it, say what to change -- "add a dark theme to
// the settings screen" -- and the architect plans it and the agents make it.
// What is particular about this project is the end: the program that made
// the change is the program changed, so it is rebuilt and started again, and
// the new one is what you are looking at.
//
// So three things, and only for a copy that has its source: where the source
// is -- the checkout the running program was built from, or one Settings
// names -- a rebuild of it, with its output kept to read, and a restart into
// what was built. A copy installed from a download has no source beside it;
// Settings says how to get one rather than pretending.
#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace crucible::self {

/// Where the source is, and the build of it.
struct Source {
    bool                  found = false;
    std::filesystem::path root;      ///< the checkout: CMakeLists.txt naming Crucible
    std::filesystem::path build;     ///< its build folder: CMakeCache.txt, or where one will go
    std::filesystem::path program;   ///< what a rebuild makes, and a restart runs
    std::string           why;       ///< why not, when not found
};

/// The checkout `chosen` names, when it is one; else the one the running
/// program was built from, found by walking up from where it is.
Source find(const std::filesystem::path& chosen = {});

/// Whether `dir` holds Crucible's source.
bool is_source(const std::filesystem::path& dir);

/// A rebuild of the source, on a thread of its own, its output kept.
class Rebuild {
public:
    Rebuild() = default;
    ~Rebuild();
    Rebuild(const Rebuild&)            = delete;
    Rebuild& operator=(const Rebuild&) = delete;

    /// Configure when there is no build folder yet, then build the program
    /// and the app runner. False, with `error`, when one is already running.
    bool start(const Source& source, std::string& error);

    bool        running() const { return running_.load(); }
    /// The exit status of the last build: 0 built, anything else failed, -1
    /// none has finished.
    int         status() const { return status_.load(); }
    /// The last lines it printed.
    std::string log() const;

private:
    void add(const std::string& line);

    std::atomic<bool>        running_{false};
    std::atomic<int>         status_{-1};
    mutable std::mutex       mutex_;
    std::vector<std::string> lines_;
    std::thread              worker_;
};

/// Start what was built, as a program of its own. The caller closes this one.
bool restart(const Source& source, std::string& error);

}  // namespace crucible::self
