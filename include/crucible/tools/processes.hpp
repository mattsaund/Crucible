// SPDX-License-Identifier: MIT
//
// Programs an expert starts and leaves running.
//
// RUN waits for a command to finish, and a development server never does: it
// is killed at the timeout with nothing to show for it. START is for those.
// The program runs on, its output is kept, and the expert can read what it
// has printed so far (LOGS), stop it (STOP), and fetch the page it serves in
// the meantime. Each gets a short name -- p1, p2 -- which is what the other
// verbs take.
//
// They belong to the session rather than to a turn or a cook: a server
// started in a cook is still serving when the chat asks about it, and all of
// them are stopped when Crucible closes, so nothing is left running behind a
// window that is gone.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crucible/util/subprocess.hpp"

namespace crucible::tools {

/// How one of them is doing, for the journal and the window.
struct ProcessInfo {
    std::string name;       ///< "p1"
    std::string command;
    bool        running = false;
    int         status  = -1;   ///< the exit status once it has one
    long        seconds = 0;    ///< since it started
};

class Processes {
public:
    Processes() = default;
    ~Processes();
    Processes(const Processes&)            = delete;
    Processes& operator=(const Processes&) = delete;

    /// Start `command` through the platform's shell in `cwd`. Returns the
    /// name it goes by, or empty with `error` set.
    std::string start(const std::string& command, const std::filesystem::path& cwd, std::string& error);

    /// The last `max_bytes` of what `name` has printed, and whether it is
    /// still running. False when there is no such process.
    bool logs(const std::string& name, std::size_t max_bytes, std::string& out, bool& running, int& status);

    /// Stop `name`. False when there is no such process.
    bool stop(const std::string& name, std::string& error);

    /// Stop everything, and wait for the readers.
    void stop_all();

    std::vector<ProcessInfo> list();

    /// How much output is kept per process. Older lines fall off the front.
    static constexpr std::size_t kKeepBytes = 256 * 1024;

private:
    struct Entry {
        std::string                       command;
        std::unique_ptr<util::Subprocess> child;
        std::thread                       reader;
        std::mutex                        mutex;    ///< guards output, running and status
        std::string                       output;
        bool                              running = true;
        int                               status  = -1;
        std::chrono::steady_clock::time_point started;
    };

    std::mutex                                     mutex_;   ///< guards the map
    std::map<std::string, std::unique_ptr<Entry>>  entries_;
    int                                            next_ = 0;
};

}  // namespace crucible::tools
