// SPDX-License-Identifier: MIT
//
// The orchestrator: routing and the cook loop, in Python, in a process of its own.
//
// DIRECTION.md's rule is each layer in the language it is good in, and
// orchestration -- deciding which expert answers, driving a cook pass after
// pass -- is where an idea should take an afternoon rather than a build. So it
// lives in scripts/orchestrator, runs on Crucible's own Python, and talks to
// the core over a pipe: one JSON object per line on its standard input and
// output. See scripts/orchestrator/crucible_orchestrator/rpc.py for the shapes.
//
// The core keeps what must not be duplicated or guessed at: which model is
// resident and on which card, what the user has trusted, the tools that touch
// the disk, the journal the window draws. The orchestrator asks for those, in
// the middle of answering what it was asked. So a call here serves the
// orchestrator's requests while it waits for the answer, on the calling thread
// -- the engine's worker, the one thread that owns the models -- and nothing
// the orchestrator asks for needs a lock it would not already have.
//
// A protocol rather than embedding an interpreter: a crash in Python is a
// process that went away and is started again on the next call, not a crash
// in the window, and the orchestrator can be driven by hand from a terminal.
#pragma once

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "crucible/util/subprocess.hpp"

namespace crucible::orchestra {

/// The protocol this build speaks. The package says which it speaks in its
/// hello, and one that disagrees is refused rather than half understood.
inline constexpr int kProtocol = 2;

/// What the core does when the orchestrator asks it something: the answer, or
/// a std::exception whose message goes back as the error.
using Handler = std::function<nlohmann::json(const std::string& method, const nlohmann::json& params)>;

class Link {
public:
    Link() = default;
    ~Link();
    Link(const Link&)            = delete;
    Link& operator=(const Link&) = delete;

    /// Start the process when it is not running. False, with `error` saying
    /// why, when there is no Python to run it on or it would not start.
    bool start(std::string& error);

    /// End the process: its input is closed, which it reads as goodbye.
    void stop();

    bool running() const;

    /// Ask the orchestrator, answering its own requests through `handler`
    /// until the answer arrives. Starts the process first if need be. Throws
    /// std::runtime_error with the reason when the call fails -- the
    /// orchestrator reported an error, or the process went away.
    ///
    /// One call at a time, from one thread.
    nlohmann::json call(const std::string& method, const nlohmann::json& params,
                        const Handler& handler);

    /// Where the process's standard error goes: data_dir()/orchestrator.log.
    static std::filesystem::path log_file();

    /// Where the package is written: data_dir()/orchestrator.
    static std::filesystem::path package_dir();

private:
    bool send(const nlohmann::json& message);
    /// The next message, or null when the process has gone.
    nlohmann::json next();
    std::string    gone(const std::string& what) const;

    std::unique_ptr<util::Subprocess> child_;
    std::thread                       reader_;

    mutable std::mutex                mutex_;   ///< guards the queue and `ended_`
    std::condition_variable           arrived_;
    std::deque<nlohmann::json>        queue_;
    bool                              ended_ = true;

    std::mutex                        call_mutex_;
    long                              next_id_ = 0;
};

/// Write the orchestrator package, compiled into this binary, into `dir`.
/// Rewritten every time the process starts, so it is always the package this
/// build expects.
bool write_package(const std::filesystem::path& dir, std::string& error);

}  // namespace crucible::orchestra
