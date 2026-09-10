// SPDX-License-Identifier: MIT
//
// Per-project session history on disk.
#include "crucible/session/store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

#include "crucible/util/platform.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

#include "crucible/config/paths.hpp"
#include "crucible/util/format.hpp"

namespace crucible {
namespace {

using json = nlohmann::json;

/// A stable, readable folder name for a project path.
///
/// The slug makes the directory browsable by a human; the hash is what makes
/// it correct, since two different checkouts can easily end their paths with
/// the same word. FNV-1a is used rather than anything cryptographic because
/// this is a lookup key, not a secret.
std::string project_key(const std::filesystem::path& root) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char c : root.string()) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }

    std::string slug = root.filename().string();
    if (slug.empty()) {
        slug = "root";
    }
    for (char& c : slug) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!safe) {
            c = '-';
        }
    }
    if (slug.size() > 40) {
        slug.resize(40);
    }

    char suffix[17] = {};
    std::snprintf(suffix, sizeof(suffix), "%016llx", static_cast<unsigned long long>(hash));
    return slug + "-" + std::string(suffix, 8);
}

std::string timestamp_id() {
    const std::time_t now = std::time(nullptr);
    std::tm parts = util::local_time(now);
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &parts);
    return buffer;
}

std::string display_time(const std::string& id) {
    // Ids are "20260830-142530"; turn one back into something readable rather
    // than storing the same instant twice.
    if (id.size() < 15) {
        return id;
    }
    return id.substr(0, 4) + "-" + id.substr(4, 2) + "-" + id.substr(6, 2) + " " +
           id.substr(9, 2) + ":" + id.substr(11, 2);
}

/// Seconds since the epoch for a session id, for "3 hours ago".
std::time_t id_to_time(const std::string& id) {
    if (id.size() < 15) {
        return 0;
    }
    std::tm parts{};
    std::istringstream stream(id);
    stream >> std::get_time(&parts, "%Y%m%d-%H%M%S");
    if (stream.fail()) {
        return 0;
    }
    parts.tm_isdst = -1;
    return std::mktime(&parts);
}

json usage_to_json(const TokenUsage& usage) {
    return json{
        {"input_tokens", usage.input_tokens},
        {"output_tokens", usage.output_tokens},
        {"turns", usage.turns},
        {"output_ms", usage.output_ms},
    };
}

TokenUsage usage_from_json(const json& obj) {
    TokenUsage usage;
    if (!obj.is_object()) {
        return usage;
    }
    usage.input_tokens  = obj.value("input_tokens", std::uint64_t{0});
    usage.output_tokens = obj.value("output_tokens", std::uint64_t{0});
    usage.turns         = obj.value("turns", std::uint64_t{0});
    usage.output_ms     = obj.value("output_ms", 0.0);
    return usage;
}

json turn_to_json(const Turn& turn) {
    json entry{
        {"prompt", turn.prompt},
        {"reply", turn.reply},
        {"output_tokens", turn.output_tokens},
        {"tokens_per_second", turn.tokens_per_second},
        {"load_ms", turn.load_ms},
        {"canceled", turn.canceled},
        {"failed", turn.failed},
    };
    if (turn.route) {
        entry["route"] = json{
            {"expert", turn.route->expert},
            {"confidence", turn.route->confidence},
            {"source", std::string(route_source_name(turn.route->source))},
            {"detail", turn.route->detail},
        };
    }
    return entry;
}

Turn turn_from_json(const json& entry) {
    Turn turn;
    turn.prompt            = entry.value("prompt", "");
    turn.reply             = entry.value("reply", "");
    turn.output_tokens     = entry.value("output_tokens", 0);
    turn.tokens_per_second = entry.value("tokens_per_second", 0.0);
    turn.load_ms           = entry.value("load_ms", 0L);
    turn.canceled         = entry.value("canceled", false);
    turn.failed            = entry.value("failed", false);

    if (const auto route = entry.find("route"); route != entry.end() && route->is_object()) {
        RouteDecision decision;
        // The id is stored verbatim and read back verbatim, with no check
        // that it still names a seat. A conversation with an expert that has
        // since been ejected is history, and history is what it was, not what
        // the expert list happens to hold today; the transcript shows the id
        // when the name is gone.
        decision.expert = route->value("expert", std::string{});
        decision.confidence = route->value("confidence", 0.0F);
        decision.detail     = route->value("detail", "");
        // Restore how it was actually routed. Defaulting this would relabel
        // every resumed turn as one the delegator declined, which is a
        // different -- and untrue -- claim about the history.
        decision.source = route_source_from_name(route->value("source", ""));
        turn.route      = decision;
    }
    return turn;
}

}  // namespace

// ---------------------------------------------------------------------------
// Project
// ---------------------------------------------------------------------------

Project Project::at(const std::filesystem::path& root) {
    // Normalized, so that /home/me/proj and /home/me/proj/ are one project
    // rather than two histories that never see each other.
    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(root, ec);
    if (ec || resolved.empty()) {
        resolved = root;
    }

    Project project;
    project.root = resolved;
    project.name = resolved.filename().string();
    if (project.name.empty()) {
        project.name = resolved.string();
    }
    project.dir = paths::projects_dir() / project_key(resolved);
    return project;
}

Project Project::current() {
    std::error_code ec;
    std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (ec) {
        cwd = ".";
    }
    return Project::at(cwd);
}

namespace {

std::filesystem::path recent_file() {
    return paths::projects_dir() / "recent.json";
}

}  // namespace

std::vector<Project> recent_projects(std::size_t limit) {
    std::vector<Project> found;
    json doc;
    try {
        std::ifstream in(recent_file());
        if (!in) {
            return found;
        }
        in >> doc;
    } catch (const json::exception&) {
        return found;  // a corrupt list is an empty list, not a failure
    }
    if (!doc.is_array()) {
        return found;
    }

    for (const json& entry : doc) {
        if (!entry.is_string()) {
            continue;
        }
        const std::filesystem::path root = entry.get<std::string>();
        std::error_code ec;
        // Dropped rather than shown grayed out. A list whose entries open onto
        // nothing is worse than a short one.
        if (!std::filesystem::is_directory(root, ec)) {
            continue;
        }
        found.push_back(Project::at(root));
        if (found.size() >= limit) {
            break;
        }
    }
    return found;
}

void remember_project(const std::filesystem::path& root) {
    const Project project = Project::at(root);

    std::vector<std::string> paths{project.root.string()};
    for (const Project& previous : recent_projects(32)) {
        if (previous.root != project.root) {
            paths.push_back(previous.root.string());
        }
    }
    if (paths.size() > 32) {
        paths.resize(32);
    }

    std::error_code ec;
    std::filesystem::create_directories(paths::projects_dir(), ec);
    std::ofstream out(recent_file());
    if (out) {
        out << json(paths).dump(2) << '\n';
    }
}

// ---------------------------------------------------------------------------
// SessionSummary
// ---------------------------------------------------------------------------

std::string SessionSummary::when() const {
    const std::time_t then = id_to_time(id);
    if (then == 0) {
        return started_at;
    }

    const std::time_t now = std::time(nullptr);
    const long seconds = static_cast<long>(std::difftime(now, then));
    if (seconds < 60)    { return "just now"; }
    if (seconds < 3600)  { return std::to_string(seconds / 60) + " min ago"; }
    if (seconds < 86400) {
        const long hours = seconds / 3600;
        return std::to_string(hours) + (hours == 1 ? " hour ago" : " hours ago");
    }
    if (seconds < 172800) { return "yesterday"; }
    if (seconds < 604800) { return std::to_string(seconds / 86400) + " days ago"; }
    return started_at.substr(0, 10);
}

// ---------------------------------------------------------------------------
// SessionStore
// ---------------------------------------------------------------------------

SessionStore::SessionStore(Project project) : project_(std::move(project)) {
    begin_new_session();
}

void SessionStore::begin_new_session() {
    session_id_ = timestamp_id();
    counted_    = TokenUsage{};
}

void SessionStore::adopt(std::string id) {
    session_id_ = std::move(id);
    // Whatever that session already spent has been folded into the project
    // total once. Recording it here stops the next save adding it again.
    std::vector<Turn> turns;
    TokenUsage usage;
    std::string error;
    if (load(session_id_, turns, usage, error)) {
        counted_ = usage;
    } else {
        counted_ = TokenUsage{};
    }
}

std::filesystem::path SessionStore::session_file(const std::string& id) const {
    return project_.dir / "sessions" / (id + ".json");
}

std::filesystem::path SessionStore::totals_file() const {
    return project_.dir / "usage.json";
}

bool SessionStore::save(const std::vector<Turn>& turns, const TokenUsage& usage,
                        std::string& error) {
    json stored_turns = json::array();
    for (const Turn& turn : turns) {
        // A reply still arriving is not resumable, and a failed turn is noise.
        if (turn.streaming || turn.reply.empty()) {
            continue;
        }
        stored_turns.push_back(turn_to_json(turn));
    }
    if (stored_turns.empty()) {
        return true;  // nothing worth a file yet
    }

    std::error_code ec;
    std::filesystem::create_directories(session_file(session_id_).parent_path(), ec);
    if (ec) {
        error = "could not create " + project_.dir.string() + ": " + ec.message();
        return false;
    }

    const json document{
        {"version", 1},
        {"id", session_id_},
        {"project", project_.root.string()},
        {"started_at", display_time(session_id_)},
        {"usage", usage_to_json(usage)},
        {"turns", stored_turns},
    };

    // Write beside the target and rename, so an interrupted save cannot leave
    // a truncated session behind in place of a good one.
    const std::filesystem::path target = session_file(session_id_);
    const std::filesystem::path temp   = target.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out) {
            error = "could not write " + temp.string();
            return false;
        }
        out << document.dump(1, '\t') << '\n';
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        error = "could not replace " + target.string() + ": " + ec.message();
        return false;
    }

    // Fold in only what is new since the last save.
    TokenUsage delta;
    delta.input_tokens  = usage.input_tokens  - std::min(usage.input_tokens,  counted_.input_tokens);
    delta.output_tokens = usage.output_tokens - std::min(usage.output_tokens, counted_.output_tokens);
    delta.turns         = usage.turns         - std::min(usage.turns,         counted_.turns);
    delta.output_ms     = std::max(0.0, usage.output_ms - counted_.output_ms);
    update_project_total(delta);
    counted_ = usage;
    return true;
}

std::vector<SessionSummary> SessionStore::list(std::size_t limit) const {
    std::vector<SessionSummary> sessions;
    const std::filesystem::path dir = project_.dir / "sessions";

    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return sessions;
    }

    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        std::ifstream in(entry.path());
        if (!in) {
            continue;
        }
        const json document = json::parse(in, nullptr, /*allow_exceptions=*/false);
        if (!document.is_object()) {
            continue;
        }

        SessionSummary summary;
        summary.id         = document.value("id", entry.path().stem().string());
        summary.started_at = document.value("started_at", display_time(summary.id));
        summary.usage      = usage_from_json(document.value("usage", json::object()));
        summary.file       = entry.path();

        if (const auto turns = document.find("turns"); turns != document.end() && turns->is_array()) {
            summary.turns = static_cast<int>(turns->size());
            if (!turns->empty()) {
                summary.title = turns->front().value("prompt", "");
            }
        }

        // One line, whatever the prompt was: newlines would break the picker's
        // row layout, and a long prompt would push the columns off screen.
        std::replace(summary.title.begin(), summary.title.end(), '\n', ' ');
        summary.title = format::trim(summary.title);
        if (summary.title.size() > 72) {
            summary.title = summary.title.substr(0, 69) + "...";
        }
        if (summary.title.empty()) {
            summary.title = "(no prompt)";
        }

        sessions.push_back(std::move(summary));
    }

    // Ids sort chronologically because they are timestamps, so newest first is
    // a plain reverse sort.
    std::sort(sessions.begin(), sessions.end(),
              [](const SessionSummary& a, const SessionSummary& b) { return a.id > b.id; });
    if (sessions.size() > limit) {
        sessions.resize(limit);
    }
    return sessions;
}

bool SessionStore::load(const std::string& id, std::vector<Turn>& turns, TokenUsage& usage,
                        std::string& error) const {
    std::ifstream in(session_file(id));
    if (!in) {
        error = "no session " + id + " for this project";
        return false;
    }

    const json document = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (!document.is_object()) {
        error = "session " + id + " is not readable";
        return false;
    }

    turns.clear();
    if (const auto stored = document.find("turns"); stored != document.end() && stored->is_array()) {
        for (const json& entry : *stored) {
            turns.push_back(turn_from_json(entry));
        }
    }
    usage = usage_from_json(document.value("usage", json::object()));
    return true;
}

bool SessionStore::remove(const std::string& id, std::string& error) const {
    std::error_code ec;
    if (!std::filesystem::remove(session_file(id), ec) || ec) {
        error = "could not delete session " + id;
        return false;
    }
    return true;
}

void SessionStore::update_project_total(const TokenUsage& delta) const {
    if (delta.total_tokens() == 0 && delta.turns == 0) {
        return;
    }

    TokenUsage total = project_usage();
    total.add(delta);

    std::error_code ec;
    std::filesystem::create_directories(project_.dir, ec);
    std::ofstream out(totals_file(), std::ios::trunc);
    if (!out) {
        return;  // a lost total is not worth interrupting the conversation for
    }
    out << json{
               {"project", project_.root.string()},
               {"usage", usage_to_json(total)},
           }
               .dump(1, '\t')
        << '\n';
}

TokenUsage SessionStore::project_usage() const {
    std::ifstream in(totals_file());
    if (!in) {
        return {};
    }
    const json document = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (!document.is_object()) {
        return {};
    }
    return usage_from_json(document.value("usage", json::object()));
}

}  // namespace crucible
