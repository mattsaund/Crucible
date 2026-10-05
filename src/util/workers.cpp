// SPDX-License-Identifier: MIT
#include "crucible/util/workers.hpp"

#include <utility>

namespace crucible::util {

Workers::Workers(std::size_t count) {
    threads_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        threads_.emplace_back([this] { run(); });
    }
}

Workers::~Workers() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
        jobs_.clear();
    }
    waiting_.notify_all();
    for (std::thread& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void Workers::post(std::function<void()> job) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (closing_) {
            return;
        }
        jobs_.push_back(std::move(job));
    }
    waiting_.notify_one();
}

void Workers::run() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            waiting_.wait(lock, [this] { return closing_ || !jobs_.empty(); });
            if (closing_) {
                return;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        // Contained. An errand that throws has failed at its own job, and
        // taking the thread -- and with it every later errand -- down with it
        // would turn one failed lookup into a window that stops answering.
        try {
            job();
        } catch (...) {
        }
    }
}

}  // namespace crucible::util
