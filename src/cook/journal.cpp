// SPDX-License-Identifier: MIT
//
// Reading and writing what a cook did.
//
// Three types in one file because they are three views of the same JSON. Cook
// is a run in full, with every step; CookSummary is the row the history list
// draws, cheap enough to read a hundred of; CookLog owns the directory they
// live in and hands out ids.
//
// The steps are appended as a cook runs rather than written at the end, so a
// crash costs the last step and not the hour before it -- which is also why a
// journal on disk can be in a running state with no process behind it.
#include "crucible/cook/journal.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>

#include "crucible/util/platform.hpp"
#include <fstream>
#include <set>
#include <system_error>

#include <nlohmann/json.hpp>

namespace crucible {
namespace {

using json = nlohmann::json;

std::string timestamp_id() {
    const std::time_t now = std::time(nullptr);
    std::tm parts = util::local_time(now);
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &parts);
    return buffer;
}

std::string minutes_phrase(std::chrono::seconds seconds) {
    const long total = static_cast<long>(seconds.count());
    if (total < 60) {
        return std::to_string(total) + (total == 1 ? " second" : " seconds");
    }
    const long minutes = total / 60;
    if (minutes < 60) {
        return std::to_string(minutes) + (minutes == 1 ? " minute" : " minutes");
    }
    const long hours = minutes / 60;
    const long rest  = minutes % 60;
    std::string out = std::to_string(hours) + (hours == 1 ? " hour" : " hours");
    if (rest > 0) {
        out += " " + std::to_string(rest) + " min";
    }
    return out;
}

std::string plural(int count, const char* one, const char* many) {
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

}  // namespace

std::optional<int> parse_duration_seconds(std::string_view text) {
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.front())) != 0)) {
        text.remove_prefix(1);
    }
    if (text.empty() || (std::isdigit(static_cast<unsigned char>(text.front())) == 0)) {
        return std::nullopt;
    }

    long value = 0;
    std::size_t i = 0;
    for (; i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) != 0); ++i) {
        value = value * 10 + (text[i] - '0');
        if (value > 100000) {
            return std::nullopt;  // not a duration anybody meant
        }
    }

    const std::string_view suffix = text.substr(i);
    if (suffix.empty() || suffix == "m" || suffix == "min" || suffix == "mins"
        || suffix == "minutes") {
        // A bare number is minutes: "give it twenty" means twenty minutes, and
        // nobody sets a cook for twenty seconds.
        return static_cast<int>(value * 60);
    }
    if (suffix == "s" || suffix == "sec" || suffix == "secs" || suffix == "seconds") {
        return static_cast<int>(value);
    }
    if (suffix == "h" || suffix == "hr" || suffix == "hrs" || suffix == "hours"
        || suffix == "hour") {
        return static_cast<int>(value * 3600);
    }
    // A number followed by something else is part of the goal, not a budget:
    // "/cook 3 tests are failing" is a sentence.
    return std::nullopt;
}

std::string format_duration(std::chrono::seconds seconds) {
    return minutes_phrase(seconds);
}

std::string_view cook_state_name(CookState state) {
    switch (state) {
        case CookState::Working:   return "working";
        case CookState::Asking:    return "asking";
        case CookState::Finishing: return "finishing";
        case CookState::Done:      return "done";
        case CookState::Stopped:   return "stopped";
        case CookState::Failed:    return "failed";
        case CookState::Idle:      break;
    }
    return "idle";
}

CookState cook_state_from_name(std::string_view name) {
    if (name == "working")   { return CookState::Working; }
    if (name == "asking")    { return CookState::Asking; }
    if (name == "finishing") { return CookState::Finishing; }
    if (name == "done")      { return CookState::Done; }
    if (name == "stopped")   { return CookState::Stopped; }
    if (name == "failed")    { return CookState::Failed; }
    return CookState::Idle;
}

// ---------------------------------------------------------------------------
// Cook
// ---------------------------------------------------------------------------

std::vector<std::string> Cook::files_touched() const {
    // First-touched order rather than sorted. It reads as the story of the
    // cook: the file it started in comes first, and the test it added last
    // comes last.
    std::vector<std::string> files;
    std::set<std::string>    seen;
    for (const CookStep& step : steps) {
        for (const std::string& path : step.changed) {
            if (seen.insert(path).second) {
                files.push_back(path);
            }
        }
    }
    return files;
}

std::vector<ExpertId> Cook::experts_used() const {
    std::vector<ExpertId> experts;
    std::set<ExpertId>    seen;
    for (const CookStep& step : steps) {
        if (!step.expert.empty() && seen.insert(step.expert).second) {
            experts.push_back(step.expert);
        }
    }
    return experts;
}

std::chrono::seconds Cook::duration() const {
    // A cook still running is measured against now, so the timer on screen
    // moves. One that has finished is measured against when it did.
    const std::int64_t end = ended_unix > 0
        ? ended_unix
        : static_cast<std::int64_t>(std::time(nullptr));
    if (started_unix <= 0 || end < started_unix) {
        return std::chrono::seconds{0};
    }
    return std::chrono::seconds{end - started_unix};
}

std::string Cook::headline() const {
    const std::size_t files = files_touched().size();
    std::string out = plural(static_cast<int>(files), "file", "files");
    out += ", " + plural(static_cast<int>(steps.size()), "step", "steps");
    out += " over " + minutes_phrase(duration());
    return out;
}

// ---------------------------------------------------------------------------
// CookSummary
// ---------------------------------------------------------------------------

std::string CookSummary::when() const {
    if (started_unix <= 0) {
        return id;
    }
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    const std::int64_t seconds = now - started_unix;
    if (seconds < 60)     { return "just now"; }
    if (seconds < 3600)   { return std::to_string(seconds / 60) + " min ago"; }
    if (seconds < 86400) {
        const std::int64_t hours = seconds / 3600;
        return std::to_string(hours) + (hours == 1 ? " hour ago" : " hours ago");
    }
    if (seconds < 172800) { return "yesterday"; }
    if (seconds < 604800) { return std::to_string(seconds / 86400) + " days ago"; }
    // Ids are "20260904-142530"; read the date back out rather than storing the
    // same instant twice.
    return id.size() >= 8
        ? id.substr(0, 4) + "-" + id.substr(4, 2) + "-" + id.substr(6, 2)
        : id;
}

// ---------------------------------------------------------------------------
// CookLog
// ---------------------------------------------------------------------------

CookLog::CookLog(std::filesystem::path project_dir)
    : project_dir_(std::move(project_dir)) {}

std::string CookLog::new_id() {
    return timestamp_id();
}

std::filesystem::path CookLog::dir() const {
    return project_dir_ / "cooks";
}

std::filesystem::path CookLog::file_for(const std::string& id) const {
    return dir() / (id + ".json");
}

json cook_to_json(const Cook& cook) {
    json steps = json::array();
    for (const CookStep& step : cook.steps) {
        steps.push_back(json{
            {"iteration", step.iteration},
            {"expert",    step.expert},
            {"kind",      step.kind},
            {"summary",   step.summary},
            {"detail",    step.detail},
            {"ok",        step.ok},
            {"ms",        step.ms},
            {"changed",   step.changed},
        });
    }

    json attachments = json::array();
    for (const attach::Tile& one : cook.attachments) {
        attachments.push_back(json{{"path", one.path}, {"name", one.name},
                                   {"label", one.label}, {"kind", one.kind}});
    }

    return json{
        {"id",             cook.id},
        {"goal",           cook.goal},
        {"attachments",    attachments},
        {"state",          std::string(cook_state_name(cook.state))},
        {"budget_seconds", cook.budget_seconds},
        {"started_unix",   cook.started_unix},
        {"ended_unix",     cook.ended_unix},
        {"iterations",     cook.iterations},
        {"outcome",        cook.outcome},
        {"question",       cook.question},
        {"steps",          steps},
    };
}

Cook cook_from_json(const json& doc, const std::string& fallback_id) {
    Cook cook;
    if (!doc.is_object()) {
        cook.id = fallback_id;
        return cook;
    }
    cook.id             = doc.value("id", fallback_id);
    if (cook.id.empty()) {
        cook.id = fallback_id;
    }
    cook.goal           = doc.value("goal", "");
    cook.state          = cook_state_from_name(doc.value("state", "done"));
    cook.budget_seconds = doc.value("budget_seconds", 0);
    cook.started_unix   = doc.value("started_unix", std::int64_t{0});
    cook.ended_unix     = doc.value("ended_unix", std::int64_t{0});
    cook.iterations     = doc.value("iterations", 0);
    cook.outcome        = doc.value("outcome", "");
    cook.question       = doc.value("question", "");

    if (const auto attachments = doc.find("attachments");
        attachments != doc.end() && attachments->is_array()) {
        for (const json& one : *attachments) {
            if (one.is_object()) {
                cook.attachments.push_back({one.value("path", ""), one.value("name", ""),
                                            one.value("label", ""), one.value("kind", "file")});
            }
        }
    }

    if (const auto steps = doc.find("steps"); steps != doc.end() && steps->is_array()) {
        for (const json& entry : *steps) {
            if (!entry.is_object()) {
                continue;
            }
            CookStep step;
            step.iteration = entry.value("iteration", 0);
            step.expert    = entry.value("expert", "");
            step.kind      = entry.value("kind", "");
            step.summary   = entry.value("summary", "");
            step.detail    = entry.value("detail", "");
            step.ok        = entry.value("ok", true);
            step.ms        = entry.value("ms", 0L);
            if (const auto changed = entry.find("changed");
                changed != entry.end() && changed->is_array()) {
                for (const json& path : *changed) {
                    if (path.is_string()) {
                        step.changed.push_back(path.get<std::string>());
                    }
                }
            }
            cook.steps.push_back(std::move(step));
        }
    }
    return cook;
}

bool CookLog::save(const Cook& cook, std::string& error) const {
    if (cook.id.empty()) {
        error = "cook has no id";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(dir(), ec);
    if (ec) {
        error = "could not create " + dir().string() + ": " + ec.message();
        return false;
    }

    const std::filesystem::path file = file_for(cook.id);
    std::ofstream out(file);
    if (!out) {
        error = "could not write " + file.string();
        return false;
    }
    out << cook_to_json(cook).dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
    return true;
}

std::vector<CookSummary> CookLog::list(std::size_t limit) const {
    std::vector<CookSummary> found;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir(), ec)) {
        return found;
    }

    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(dir(), ec)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        json doc;
        try {
            std::ifstream in(entry.path());
            in >> doc;
        } catch (const json::exception&) {
            continue;  // one unreadable file must not hide the rest
        }
        if (!doc.is_object()) {
            continue;
        }

        CookSummary summary;
        summary.id           = doc.value("id", entry.path().stem().string());
        summary.goal         = doc.value("goal", "");
        summary.state        = cook_state_from_name(doc.value("state", "done"));
        summary.started_unix = doc.value("started_unix", std::int64_t{0});
        summary.iterations   = doc.value("iterations", 0);
        summary.file         = entry.path();

        const std::int64_t ended = doc.value("ended_unix", std::int64_t{0});
        summary.duration = std::chrono::seconds{
            ended > summary.started_unix ? ended - summary.started_unix : 0};

        std::set<std::string> files;
        if (const auto steps = doc.find("steps"); steps != doc.end() && steps->is_array()) {
            summary.steps = static_cast<int>(steps->size());
            for (const json& step : *steps) {
                if (const auto changed = step.find("changed");
                    changed != step.end() && changed->is_array()) {
                    for (const json& path : *changed) {
                        if (path.is_string()) {
                            files.insert(path.get<std::string>());
                        }
                    }
                }
            }
        }
        summary.files = static_cast<int>(files.size());
        found.push_back(std::move(summary));
    }

    // Newest first: ids are timestamps, so this is a plain reverse sort.
    std::sort(found.begin(), found.end(),
              [](const CookSummary& a, const CookSummary& b) { return a.id > b.id; });
    if (found.size() > limit) {
        found.resize(limit);
    }
    return found;
}

std::optional<Cook> CookLog::load(const std::string& id) const {
    json doc;
    try {
        std::ifstream in(file_for(id));
        if (!in) {
            return std::nullopt;
        }
        in >> doc;
    } catch (const json::exception&) {
        return std::nullopt;
    }
    if (!doc.is_object()) {
        return std::nullopt;
    }
    return cook_from_json(doc, id);
}

bool CookLog::remove(const std::string& id, std::string& error) const {
    std::error_code ec;
    if (!std::filesystem::remove(file_for(id), ec)) {
        error = ec ? ec.message() : "no cook called " + id;
        return false;
    }
    return true;
}

}  // namespace crucible
