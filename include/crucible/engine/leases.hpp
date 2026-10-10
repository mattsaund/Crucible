// SPDX-License-Identifier: MIT
//
// Which models on this machine are in use, so none is freed under its user.
//
// One local model at a time was the whole memory argument of the engine: an
// expert is freed before the next is loaded, so the peak is the larger of
// the two and never their sum. That holds as long as one thing at a time
// wants a model. A build's agents and the chat want them together -- one
// agent on a local coder while another waits on Claude, the chat asking a
// question while the build works -- and freeing the model one of them is
// halfway through a sentence with is not a memory policy, it is a crash.
//
// So a model in use is leased: a lease is taken when a seat takes the model
// and dropped when the seat lets it go, and the host frees only what nobody
// holds a lease on. When a model is wanted that will not fit beside the ones
// leased, the asker waits for a lease to end rather than pushing a model out
// from under somebody. With nothing leased, nothing changes: the next model
// still frees the last one first.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace crucible {

class Leases {
public:
    /// One user's hold on one model, by its key -- the model's path. Moved,
    /// never copied; dropping it ends the hold.
    class Lease {
    public:
        Lease() = default;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        Lease(const Lease&)            = delete;
        Lease& operator=(const Lease&) = delete;
        ~Lease();

        bool               held() const { return owner_ != nullptr; }
        const std::string& key() const { return key_; }

        /// End the hold now rather than when this goes out of scope.
        void release();

    private:
        friend class Leases;
        Lease(Leases* owner, std::string key) : owner_(owner), key_(std::move(key)) {}

        Leases*     owner_ = nullptr;
        std::string key_;
    };

    /// Hold `key`. Never waits: whether the model may be loaded is the
    /// host's question, asked before this; this only records that it is in
    /// use. Take it before letting go of whatever lock the load was made
    /// under, or the model can be freed in between.
    Lease take(const std::string& key);

    /// Whether anybody holds `key`.
    bool held(const std::string& key) const;

    /// How many hold it.
    int count(const std::string& key) const;

    /// How many leases have ended, ever. Read before asking the host for a
    /// model, and handed to wait_for_release, so a lease that ends between
    /// the asking and the waiting is not missed.
    std::uint64_t releases() const;

    /// Wait until a lease has ended since `since`, `cancel` says to stop, or
    /// `longest` passes. True when one ended -- the moment to ask the host
    /// again.
    bool wait_for_release(const std::function<bool()>& cancel, std::uint64_t since,
                          std::chrono::milliseconds longest = std::chrono::minutes(10));

    /// Wake every waiter, so it looks at its cancel switch. For shutting down.
    void wake_all();

private:
    void drop(const std::string& key);

    mutable std::mutex         mutex_;
    std::condition_variable    released_;
    std::map<std::string, int> counts_;
    std::uint64_t              releases_ = 0;   ///< how many have ended, ever
};

}  // namespace crucible
