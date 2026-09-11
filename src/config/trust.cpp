// SPDX-License-Identifier: MIT
//
// The folder-trust store.
//
// Deliberately forgiving: a missing or corrupt store means "nothing is trusted
// yet", never an error. The worst case is one extra prompt, whereas refusing to
// start over a malformed JSON file would be unforgivable.
#include "crucible/config/trust.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>
#include <nlohmann/json.hpp>

namespace crucible {
namespace {

using json = nlohmann::json;

std::filesystem::path normalize(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(dir, ec);
    return ec ? dir : resolved;
}

}  // namespace

TrustStore::TrustStore(std::filesystem::path file) : file_(std::move(file)) {
    if (!std::filesystem::exists(file_)) {
        return;
    }

    json doc;
    try {
        std::ifstream in(file_);
        in >> doc;
    } catch (const json::exception&) {
        return;  // corrupt store: start over rather than refuse to run
    }

    const auto it = doc.find("trusted");
    if (it == doc.end() || !it->is_array()) {
        return;
    }
    for (const json& entry : *it) {
        if (entry.is_object()) {
            if (const auto path = entry.find("path");
                path != entry.end() && path->is_string()) {
                trusted_.emplace_back(path->get<std::string>());
            }
        } else if (entry.is_string()) {
            trusted_.emplace_back(entry.get<std::string>());
        }
    }
}

bool TrustStore::is_trusted(const std::filesystem::path& dir) const {
    const std::filesystem::path target = normalize(dir);

    return std::any_of(trusted_.begin(), trusted_.end(),
                       [&](const std::filesystem::path& entry) {
        if (entry == target) {
            return true;
        }
        // Walk up from the target looking for a trusted ancestor. Comparing
        // whole path components avoids "/home/matt/foo" matching "/home/matt/foobar".
        auto parent = target.parent_path();
        while (!parent.empty()) {
            if (parent == entry) {
                return true;
            }
            const auto next = parent.parent_path();
            if (next == parent) {
                break;  // reached the root
            }
            parent = next;
        }
        return false;
    });
}

bool TrustStore::trust(const std::filesystem::path& dir) {
    const std::filesystem::path target = normalize(dir);
    if (std::find(trusted_.begin(), trusted_.end(), target) == trusted_.end()) {
        trusted_.push_back(target);
    }
    return save();
}

bool TrustStore::save() const {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    json entries = json::array();
    for (const std::filesystem::path& entry : trusted_) {
        entries.push_back(json{{"path", entry.string()}, {"added", now}});
    }

    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);

    std::ofstream out(file_);
    if (!out) {
        return false;
    }
    out << json{{"trusted", entries}}.dump(2) << '\n';
    return out.good();
}

}  // namespace crucible
