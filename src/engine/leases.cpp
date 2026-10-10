// SPDX-License-Identifier: MIT
//
// See leases.hpp.
#include "crucible/engine/leases.hpp"

#include <utility>

namespace crucible {

Leases::Lease::Lease(Lease&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), key_(std::move(other.key_)) {}

Leases::Lease& Leases::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
        key_   = std::move(other.key_);
    }
    return *this;
}

Leases::Lease::~Lease() { release(); }

void Leases::Lease::release() {
    if (Leases* owner = std::exchange(owner_, nullptr)) {
        owner->drop(key_);
    }
}

Leases::Lease Leases::take(const std::string& key) {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++counts_[key];
    return Lease(this, key);
}

bool Leases::held(const std::string& key) const { return count(key) > 0; }

int Leases::count(const std::string& key) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = counts_.find(key);
    return found == counts_.end() ? 0 : found->second;
}

void Leases::drop(const std::string& key) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = counts_.find(key);
        if (found != counts_.end() && --found->second <= 0) {
            counts_.erase(found);
        }
        ++releases_;
    }
    released_.notify_all();
}

std::uint64_t Leases::releases() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return releases_;
}

bool Leases::wait_for_release(const std::function<bool()>& cancel, std::uint64_t since,
                              std::chrono::milliseconds longest) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto deadline = std::chrono::steady_clock::now() + longest;
    // Woken by a release, by wake_all, and every quarter second besides: the
    // cancel switch is somebody else's atomic, and nobody notifies this
    // condition when it flips.
    while (releases_ == since) {
        if (cancel && cancel()) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        released_.wait_for(lock, std::chrono::milliseconds(250));
    }
    return true;
}

void Leases::wake_all() { released_.notify_all(); }

}  // namespace crucible
