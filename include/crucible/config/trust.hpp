// SPDX-License-Identifier: MIT
// The "do you trust this folder?" gate.
//
// Crucible is meant to be run by cd-ing into a project and typing `crucible`, and
// it will eventually read and write files there. Trust is therefore recorded
// per directory and asked for exactly once.
#pragma once

#include <filesystem>
#include <vector>

namespace crucible {

class TrustStore {
public:
    /// Load from `file`. A missing or corrupt store is treated as "nothing is
    /// trusted yet" rather than an error -- the worst case is one extra prompt.
    explicit TrustStore(std::filesystem::path file);

    /// True when `dir` itself, or any parent of it, has been trusted.
    /// Trusting ~/code therefore covers ~/code/project without re-asking.
    bool is_trusted(const std::filesystem::path& dir) const;

    /// Record `dir` as trusted and persist immediately, so a crash later in
    /// startup cannot lose the answer the user just gave.
    bool trust(const std::filesystem::path& dir);

    /// Forget every folder, so each is asked about again when it is opened.
    /// Persisted at once, as trust() is.
    bool clear();

    const std::vector<std::filesystem::path>& entries() const { return trusted_; }

private:
    bool save() const;

    std::filesystem::path              file_;
    std::vector<std::filesystem::path> trusted_;
};

}  // namespace crucible
