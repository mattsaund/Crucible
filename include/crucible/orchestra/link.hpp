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
// the middle of answering what it was asked.
//
// Both sides may have many calls outstanding at once. The chat's routing and
// a build run side by side, and a build's agents each ask for their own
// rounds, so a call is matched to its answer by id rather than by being the
// only one waiting, and every request the orchestrator makes is answered on a
// thread of its own: one agent waiting on its model does not hold up another
// agent's tool, or the chat's routing. What a handler touches is the engine's
// to guard, and it does -- see engine.hpp.
//
// A protocol rather than embedding an interpreter: a crash in Python is a
// process that went away and is started again on the next call, not a crash
// in the window, and the orchestrator can be driven by hand from a terminal.
#pragma once

#include <condition_variable>
#include <atomic>
#include <list>
#include <map>
#include <optional>
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

    /// What answers the orchestrator's requests. Set before the first call;
    /// it is called on threads of the link's own, any number at once.
    void set_handler(Handler handler);

    /// Start the process when it is not running. False, with `error` saying
    /// why, when there is no Python to run it on or it would not start.
    bool start(std::string& error);

    /// End the process: its input is closed, which it reads as goodbye. Calls
    /// waiting on it fail, and the threads answering its requests are joined
    /// -- whatever they are waiting on must have been woken first.
    void stop();

    bool running() const;

    /// Ask the orchestrator and wait for the answer. Starts the process first
    /// if need be. Throws std::runtime_error with the reason when the call
    /// fails -- the orchestrator reported an error, or the process went away.
    ///
    /// From any thread, as many at once as there are threads to make them.
    nlohmann::json call(const std::string& method, const nlohmann::json& params);

    /// Where the process's standard error goes: data_dir()/orchestrator.log.
    static std::filesystem::path log_file();

    /// Where the package is written: data_dir()/orchestrator.
    static std::filesystem::path package_dir();

private:
    /// One call waiting for its answer.
    struct Waiter {
        bool           done = false;
        nlohmann::json message;
    };

    bool send(const nlohmann::json& message);
    void read_all(util::Subprocess* child);
    void answer(nlohmann::json request);
    void reap();
    std::string gone(const std::string& what) const;

    Handler handler_;

    std::mutex                        lifecycle_mutex_;   ///< start and stop, one at a time
    std::unique_ptr<util::Subprocess> child_;
    std::thread                       reader_;

    std::mutex                        send_mutex_;        ///< one line on the pipe at a time

    mutable std::mutex                mutex_;   ///< guards everything below
    std::condition_variable           arrived_;
    std::map<long, Waiter*>           waiting_;
    std::optional<nlohmann::json>     hello_;
    bool                              ended_ = true;
    long                              next_id_ = 0;

    /// The threads answering the orchestrator's requests, and whether each
    /// has finished, so a finished one is joined when the next starts.
    struct Answering {
        std::thread       thread;
        std::atomic<bool> finished{false};
    };
    std::list<std::unique_ptr<Answering>> answering_;
};

/// Write the orchestrator package, compiled into this binary, into `dir`.
/// Rewritten every time the process starts, so it is always the package this
/// build expects.
bool write_package(const std::filesystem::path& dir, std::string& error);

}  // namespace crucible::orchestra
