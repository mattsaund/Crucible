// SPDX-License-Identifier: MIT
//
// A few threads to hand slow errands to.
//
// The window has one thread that draws, and anything that runs on it for a
// second is a second the window does not answer. Most of what an interface
// asks for is instant. Some of it is not -- scanning a models directory on a
// slow disk, asking Python what it has installed, a request to somebody's API
// -- and those go here instead.
//
// Deliberately small. It is not a scheduler: there is no priority, no result
// type, no cancellation. A job is a function, it runs once, and whatever it
// has to say it says through what it captured.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace crucible::util {

class Workers {
public:
    /// `count` threads, started now. More than one because the errands are
    /// mostly waiting -- on a disk, on a child process, on the network -- and
    /// one slow one should not hold up the rest.
    explicit Workers(std::size_t count = 4);

    /// Finishes what is running and discards what had not started. A job that
    /// was never begun has nobody waiting on it by the time this runs.
    ~Workers();

    Workers(const Workers&)            = delete;
    Workers& operator=(const Workers&) = delete;

    /// Run `job` on one of the threads. Does nothing once the pool is closing.
    void post(std::function<void()> job);

private:
    void run();

    std::mutex                        mutex_;
    std::condition_variable           waiting_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread>          threads_;
    bool                              closing_ = false;
};

}  // namespace crucible::util
