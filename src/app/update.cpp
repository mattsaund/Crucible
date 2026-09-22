// SPDX-License-Identifier: MIT
#include "crucible/app/update.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "crucible/config/paths.hpp"
#include "crucible/util/subprocess.hpp"

#ifndef CRUCIBLE_VERSION
#define CRUCIBLE_VERSION "0.0.0"
#endif

namespace crucible::update {
namespace {

using json = nlohmann::json;

/// The repository releases are published from. One copy, because a wrong URL
/// here is an update notice nobody ever sees rather than an error anybody sees.
constexpr const char* kRepository = "mattsaund/Crucible";

/// Split "1.2.3" into its numbers, ignoring a leading `v` and stopping at the
/// first thing that is not a digit or a dot. "0.5.0-rc1" gives {0, 5, 0}, and
/// the suffix is handled separately by compare().
std::vector<int> parts_of(std::string_view text) {
    std::vector<int> parts;
    std::size_t i = 0;
    if (i < text.size() && (text[i] == 'v' || text[i] == 'V')) {
        ++i;
    }
    int value = 0;
    bool digits = false;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            value = value * 10 + (c - '0');
            digits = true;
        } else if (c == '.') {
            parts.push_back(digits ? value : 0);
            value = 0;
            digits = false;
        } else {
            break;  // a pre-release suffix, or trailing prose
        }
    }
    if (digits) {
        parts.push_back(value);
    }
    return parts;
}

/// Whether a version carries a pre-release suffix: 0.5.0-rc1 is older than
/// 0.5.0, which is the one rule that makes a release candidate safe to publish.
bool pre_release(std::string_view text) {
    return text.find('-') != std::string_view::npos;
}

std::int64_t now_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

std::string_view current() {
    return CRUCIBLE_VERSION;
}

int compare(std::string_view a, std::string_view b) {
    const std::vector<int> left  = parts_of(a);
    const std::vector<int> right = parts_of(b);
    const std::size_t count = std::max(left.size(), right.size());
    for (std::size_t i = 0; i < count; ++i) {
        const int l = i < left.size() ? left[i] : 0;
        const int r = i < right.size() ? right[i] : 0;
        if (l != r) {
            return l < r ? -1 : 1;
        }
    }
    // Same numbers. A pre-release of a version is older than the version.
    const bool lp = pre_release(a);
    const bool rp = pre_release(b);
    if (lp == rp) {
        return 0;
    }
    return lp ? -1 : 1;
}

std::string releases_url() {
    return std::string("https://api.github.com/repos/") + kRepository
         + "/releases/latest";
}

bool parse_latest(std::string_view body, std::string& version, std::string& page) {
    const json parsed = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_object()) {
        return false;
    }
    const auto tag = parsed.find("tag_name");
    if (tag == parsed.end() || !tag->is_string()) {
        return false;  // a rate-limit note, an error, or a repo with no releases
    }
    version = tag->get<std::string>();
    if (version.empty()) {
        return false;
    }
    if (const auto url = parsed.find("html_url"); url != parsed.end() && url->is_string()) {
        page = url->get<std::string>();
    } else {
        page = std::string("https://github.com/") + kRepository + "/releases";
    }
    return true;
}

std::filesystem::path cache_file() {
    return paths::data_dir() / "update.json";
}

bool read_cache(State& state) {
    std::ifstream in(cache_file());
    if (!in) {
        return false;
    }
    const json parsed = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_object()) {
        return false;
    }
    state.latest     = parsed.value("latest", "");
    state.page       = parsed.value("page", "");
    state.checked_at = parsed.value("checked_at", std::int64_t{0});
    return true;
}

bool write_cache(const State& state) {
    std::error_code ec;
    std::filesystem::create_directories(cache_file().parent_path(), ec);
    std::ofstream out(cache_file());
    if (!out) {
        return false;
    }
    const json doc{
        {"latest", state.latest},
        {"page", state.page},
        {"checked_at", state.checked_at},
    };
    out << doc.dump(2) << '\n';
    return out.good();
}

bool due(const State& state, std::int64_t now, std::int64_t every) {
    // A clock that has gone backwards -- a restored backup, a machine that was
    // wrong about the year until NTP corrected it -- would otherwise park the
    // next check somewhere in the future and never check again.
    if (state.checked_at > now) {
        return true;
    }
    return now - state.checked_at >= every;
}

bool newer_than_this(const State& state) {
    return !state.latest.empty() && compare(state.latest, current()) > 0;
}

std::string_view update_command() {
#if defined(_WIN32)
    return "irm https://raw.githubusercontent.com/mattsaund/Crucible/main/install.ps1 | iex";
#else
    return "curl -fsSL https://raw.githubusercontent.com/mattsaund/Crucible/main/install.sh | bash";
#endif
}

bool fetch(std::string& body, std::string& error) {
    if (!util::on_path("curl")) {
        error = "curl is needed to check for updates and is not installed";
        return false;
    }
    const std::vector<std::string> argv{
        "curl", "--silent", "--show-error", "--location", "--fail",
        "--max-time", "10",
        "--user-agent", std::string("Crucible/") + CRUCIBLE_VERSION,
        "--header", "Accept: application/vnd.github+json",
        releases_url(),
    };
    util::Subprocess child;
    if (!child.start(argv, {}, /*extra_env=*/{}, error)) {
        return false;
    }
    std::string line;
    while (child.read_line(line)) {
        body += line;
        body += '\n';
    }
    if (const int status = child.wait(); status != 0) {
        error = "the release list could not be reached (curl exited "
              + std::to_string(status) + ")";
        return false;
    }
    return true;
}

State refresh(bool allowed_to_ask) {
    State state;
    read_cache(state);
    if (!allowed_to_ask || !due(state, now_seconds())) {
        return state;
    }

    std::string body;
    std::string error;
    if (!fetch(body, error)) {
        // A failed check is not news. Remember that it was tried, so a machine
        // with no network does not spawn a check on every start.
        state.checked_at = now_seconds();
        write_cache(state);
        return state;
    }

    std::string version;
    std::string page;
    if (parse_latest(body, version, page)) {
        state.latest = version;
        state.page   = page;
    }
    state.checked_at = now_seconds();
    write_cache(state);
    return state;
}

}  // namespace crucible::update
