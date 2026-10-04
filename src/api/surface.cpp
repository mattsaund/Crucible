// SPDX-License-Identifier: MIT
#include "crucible/api/surface.hpp"

#include "crucible/config/paths.hpp"
#include "crucible/lab/recipe.hpp"
#include "crucible/llm/model_catalog.hpp"
#include "crucible/runtime/backend.hpp"
#include "crucible/runtime/registry.hpp"
#include "crucible/session/store.hpp"

#include <algorithm>
#include <map>

#include <nlohmann/json.hpp>

namespace crucible::api {
namespace {

using json = nlohmann::json;

// --- the vocabulary -------------------------------------------------------
//
// Enums cross the boundary as words, not numbers. A number is a thing the two
// sides have to agree about forever and cannot be read in a log; a word
// survives a reordering of the enum and says what it means when a request is
// printed out.

std::string_view mood_word(Mood mood) {
    switch (mood) {
        case Mood::Routing:  return "routing";
        case Mood::Loading:  return "loading";
        case Mood::Thinking: return "thinking";
        case Mood::Talking:  return "talking";
        case Mood::Error:    return "error";
        case Mood::Idle:     break;
    }
    return "idle";
}

std::string_view seat_word(SeatPhase phase) {
    switch (phase) {
        case SeatPhase::Missing:      return "missing";
        case SeatPhase::Dormant:      return "dormant";
        case SeatPhase::Loading:      return "loading";
        case SeatPhase::Active:       return "active";
        case SeatPhase::Unconfigured: break;
    }
    return "unconfigured";
}

json usage_json(const TokenUsage& usage) {
    return json{
        {"input_tokens",  usage.input_tokens},
        {"output_tokens", usage.output_tokens},
        {"turns",         usage.turns},
        {"output_ms",     usage.output_ms},
    };
}

json turn_json(const Turn& turn) {
    json out{
        {"prompt",            turn.prompt},
        {"reply",             turn.reply},
        {"streaming",         turn.streaming},
        {"canceled",          turn.canceled},
        {"failed",            turn.failed},
        {"tokens_per_second", turn.tokens_per_second},
    };
    // Reasoning is sent but kept apart from the reply, for the same reason the
    // window draws it apart: it is not the answer, and an interface that
    // concatenated them would make a reasoning model look broken.
    if (!turn.reasoning.empty()) {
        out["reasoning"] = turn.reasoning;
    }
    if (turn.route) {
        out["route"] = json{
            {"expert",     turn.route->expert},
            {"confidence", turn.route->confidence},
        };
    }
    if (!turn.actions.empty()) {
        json actions = json::array();
        for (const TurnAction& action : turn.actions) {
            json one{{"summary", action.summary}};
            if (!action.body.empty())     { one["body"]     = action.body; }
            if (!action.language.empty()) { one["language"] = action.language; }
            actions.push_back(std::move(one));
        }
        out["actions"] = std::move(actions);
    }
    return out;
}

/// The whole drawable state.
///
/// Sent entire rather than as a diff. A snapshot is a few kilobytes, it is
/// produced once per wake rather than per token, and a diff protocol would
/// buy some bandwidth across an in-process call in exchange for two sides
/// that can disagree about what they are looking at.
json snapshot_to_json(const Snapshot& snapshot) {
    json out{
        {"mood",             mood_word(snapshot.mood)},
        {"status",           snapshot.status},
        {"busy",             snapshot.busy},
        {"delegator_ready",  snapshot.delegator_ready},
        {"context_used",     snapshot.context_used},
        {"context_size",     snapshot.context_size},
        {"tokens_per_second", snapshot.live_tokens_per_second},
        {"session_usage",    usage_json(snapshot.session_usage)},
        {"project_usage",    usage_json(snapshot.project_usage)},
        {"notices",          snapshot.notices},
    };

    if (snapshot.resident) { out["resident"] = *snapshot.resident; }
    if (snapshot.linked)   { out["linked"]   = *snapshot.linked; }

    // The roster and the seat states are parallel arrays in the engine and one
    // array of objects here. Two arrays that have to be zipped by index is a
    // thing every interface would have to get right separately.
    json experts = json::array();
    if (snapshot.roster) {
        const std::vector<Expert>& list = snapshot.roster->experts();
        for (std::size_t i = 0; i < list.size(); ++i) {
            json one{
                {"id",    list[i].id},
                {"name",  list[i].name},
                {"tag",   list[i].tag},
                {"blurb", list[i].blurb},
                {"phase", i < snapshot.seats.size() ? seat_word(snapshot.seats[i].phase)
                                                    : seat_word(SeatPhase::Unconfigured)},
                {"progress", i < snapshot.seats.size() ? snapshot.seats[i].progress : 0.0F},
            };
            experts.push_back(std::move(one));
        }
    }
    out["experts"] = std::move(experts);

    json turns = json::array();
    for (const Turn& turn : snapshot.turns) {
        turns.push_back(turn_json(turn));
    }
    out["turns"] = std::move(turns);

    if (snapshot.cook) {
        out["cook"] = json{
            {"running", true},
            {"steps",   snapshot.cook->steps.size()},
        };
    }
    if (snapshot.pending_edit) {
        // Both sides of the edit, because the interface draws them side by
        // side and asking for them in a second request would mean the file
        // could change between the two.
        out["pending_edit"] = json{
            {"path",   snapshot.pending_edit->path},
            {"before", snapshot.pending_edit->before},
            {"after",  snapshot.pending_edit->after},
        };
    }
    return out;
}

json ok(const json& id, json result) {
    return json{{"id", id}, {"ok", true}, {"result", std::move(result)}};
}

json fail(const json& id, std::string message) {
    return json{{"id", id}, {"ok", false}, {"error", std::move(message)}};
}

}  // namespace

std::string snapshot_json(const Snapshot& snapshot) {
    return snapshot_to_json(snapshot).dump();
}

std::vector<std::string> Surface::methods() {
    return {
        "ping", "methods", "snapshot",
        "submit", "cancel", "release",
        "cook.start", "cook.stop", "cook.answer",
        "edit.approve",
        "config", "config.set",
        "models", "runtimes",
        "project", "project.open", "projects", "browse", "trust.answer",
    };
}

std::string Surface::handle(std::string_view request) {
    const json parsed = json::parse(request, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return fail(nullptr, "not a request object").dump();
    }

    const json id     = parsed.contains("id") ? parsed["id"] : json(nullptr);
    const auto method = parsed.value("method", std::string{});
    const json params = parsed.contains("params") && parsed["params"].is_object()
                            ? parsed["params"]
                            : json::object();

    if (method.empty()) {
        return fail(id, "no method named").dump();
    }
    // These two answer before the engine is looked at, because they are what
    // a caller asks to find out whether anything is listening at all. A door
    // that cannot say "open" until the house is finished is not much of a
    // door.
    if (method == "ping") {
        return ok(id, json{{"version", CRUCIBLE_VERSION}}).dump();
    }
    if (method == "methods") {
        return ok(id, methods()).dump();
    }

    // Is this a method at all? Asked before the engine is, because whether a
    // name exists is a fact about this surface and not about what is running
    // behind it -- and a caller with a typo told "the engine is not running"
    // goes and debugs the wrong thing entirely.
    const std::vector<std::string> known = methods();
    if (std::find(known.begin(), known.end(), method) == known.end()) {
        return fail(id, "no method called '" + method + "'").dump();
    }

    // Everything past here moves the engine, so a null one is a sentence
    // rather than a crash.
    if (deps_.engine == nullptr || deps_.state == nullptr) {
        return fail(id, "the engine is not running").dump();
    }

    if (method == "snapshot") {
        return ok(id, snapshot_to_json(deps_.state->snapshot())).dump();
    }

    if (method == "submit") {
        const auto prompt = params.value("prompt", std::string{});
        if (prompt.empty()) {
            return fail(id, "submit needs a prompt").dump();
        }
        // An expert named here skips routing, which is what `/physics ...`
        // does in the window. An empty one is not an error: it is the normal
        // case, and it means "let the delegator decide".
        const auto expert = params.value("expert", std::string{});
        deps_.engine->submit(prompt, expert.empty() ? std::nullopt
                                                    : std::optional<ExpertId>(expert));
        return ok(id, json::object()).dump();
    }
    if (method == "cancel") {
        deps_.engine->cancel();
        return ok(id, json::object()).dump();
    }
    if (method == "release") {
        if (params.value("all", false)) {
            deps_.engine->release_all();
        } else {
            deps_.engine->release_expert();
        }
        return ok(id, json::object()).dump();
    }

    if (method == "cook.start") {
        const auto goal = params.value("goal", std::string{});
        if (goal.empty()) {
            return fail(id, "cook.start needs a goal").dump();
        }
        // A cook reads and writes files, so it needs somewhere it is allowed
        // to. Refused here rather than in each interface, because "which
        // folder has been trusted" is the session's answer and not theirs.
        const std::filesystem::path root =
            deps_.project_root ? deps_.project_root() : std::filesystem::path{};
        if (root.empty()) {
            return fail(id, "no project is open, and a cook works on a project").dump();
        }
        deps_.engine->start_cook(goal, params.value("seconds", 0), root);
        return ok(id, json::object()).dump();
    }
    if (method == "cook.stop") {
        // Not a cancel: the cook stops taking new work and makes a finishing
        // pass. The distinction is the whole reason stop_cook exists.
        deps_.engine->stop_cook();
        return ok(id, json::object()).dump();
    }
    if (method == "cook.answer") {
        deps_.engine->answer_cook(params.value("answer", std::string{}));
        return ok(id, json::object()).dump();
    }

    // --- what the settings screen needs ------------------------------------

    if (method == "config") {
        if (!deps_.config) {
            return fail(id, "no configuration is loaded").dump();
        }
        // Parsed and re-embedded rather than passed through as a string: an
        // interface asking for the configuration wants an object, and the one
        // serializer in config_io.cpp is the only thing that should know its
        // shape.
        const json document = json::parse(config_to_json_text(deps_.config()),
                                          nullptr, false);
        return ok(id, document.is_object() ? document : json::object()).dump();
    }

    if (method == "config.set") {
        if (!deps_.config || !deps_.apply_config) {
            return fail(id, "the configuration cannot be changed from here").dump();
        }
        Config edited = deps_.config();
        // A patch rather than a whole document. An interface that sent the
        // config back entire would overwrite every field it does not draw,
        // which is how a settings screen silently resets things it never
        // showed anyone.
        if (params.contains("models_dir") && params["models_dir"].is_string()) {
            edited.models_dir = params["models_dir"].get<std::string>();
        }
        if (params.contains("reasoning_effort") && params["reasoning_effort"].is_string()) {
            edited.reasoning_effort = params["reasoning_effort"].get<std::string>();
        }
        if (params.contains("system_prompt") && params["system_prompt"].is_string()) {
            edited.system_prompt = params["system_prompt"].get<std::string>();
        }
        if (params.contains("router_model") && params["router_model"].is_string()) {
            edited.router.model = params["router_model"].get<std::string>();
        }
        // {"experts": {"mathematics": "a-model.gguf"}} -- the seat an id names
        // keeps everything else it had.
        if (params.contains("experts") && params["experts"].is_object()) {
            for (const auto& [seat, model] : params["experts"].items()) {
                if (model.is_string()) {
                    edited.experts[seat].model = model.get<std::string>();
                }
            }
        }
        const std::string error = deps_.apply_config(std::move(edited));
        return error.empty() ? ok(id, json::object()).dump() : fail(id, error).dump();
    }

    if (method == "models") {
        // What a seat can be pointed at: the models directory, and what the
        // lab has finished. Both, because a fine-tune made here is a model
        // and having to go and find its file would make the Create tab a
        // detour.
        json out = json::array();
        if (deps_.config) {
            for (const ModelFile& file : scan_models(deps_.config().resolved_models_dir())) {
                out.push_back(json{{"name", file.name},
                                   {"path", file.path.string()},
                                   {"bytes", file.bytes},
                                   {"made_here", false}});
            }
        }
        for (const lab::Made& made : lab::finished_models()) {
            out.push_back(json{{"name", made.name},
                               {"path", made.path.string()},
                               {"bytes", made.bytes},
                               {"made_here", true}});
        }
        return ok(id, out).dump();
    }

    if (method == "runtimes") {
        json out = json::array();
        for (const RuntimeStatus& status : RuntimeRegistry::scan()) {
            out.push_back(json{
                {"id",        std::string(backend_info(status.kind).id)},
                {"installed", status.installed},
                {"active",    status.active},
                {"devices",   status.device_count},
                {"bytes",     status.bytes},
                {"stale",     status.stale},
                {"source",    status.source},
            });
        }
        return ok(id, out).dump();
    }

    // --- which folder we are working in ------------------------------------

    if (method == "project") {
        const std::filesystem::path root =
            deps_.project_root ? deps_.project_root() : std::filesystem::path{};
        const std::filesystem::path asking =
            deps_.pending_trust ? deps_.pending_trust() : std::filesystem::path{};
        json out{{"open", !root.empty()},
                 {"root", root.string()},
                 {"name", root.empty() ? "" : root.filename().string()}};
        // Reported with the project rather than as its own method, because
        // "which folder are we in" and "which folder is waiting to be allowed"
        // are one question asked at one moment.
        if (!asking.empty()) {
            out["pending_trust"] = asking.string();
        }
        return ok(id, out).dump();
    }
    if (method == "trust.answer") {
        if (!deps_.answer_trust) {
            return fail(id, "there is nothing to answer").dump();
        }
        deps_.answer_trust(params.value("trusted", false));
        return ok(id, json::object()).dump();
    }
    if (method == "projects") {
        json out = json::array();
        for (const Project& project : recent_projects()) {
            out.push_back(json{{"root", project.root.string()}, {"name", project.name}});
        }
        return ok(id, out).dump();
    }
    if (method == "project.open") {
        if (!deps_.open_project) {
            return fail(id, "no project can be opened from here").dump();
        }
        const auto path = params.value("path", std::string{});
        if (path.empty()) {
            return fail(id, "project.open needs a path").dump();
        }
        const std::string error = deps_.open_project(std::filesystem::path(path));
        return error.empty() ? ok(id, json::object()).dump() : fail(id, error).dump();
    }

    if (method == "browse") {
        // The folder picker's one question. Hidden directories are left out,
        // which is what every file dialog does and what makes the list
        // readable in a home directory.
        const auto where = params.value("path", std::string{});
        std::filesystem::path at = where.empty() ? paths::expand_user("~")
                                                 : paths::expand_user(where);
        std::error_code ec;
        if (!std::filesystem::is_directory(at, ec)) {
            return fail(id, at.string() + " is not a directory").dump();
        }
        json entries = json::array();
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(
                 at, std::filesystem::directory_options::skip_permission_denied, ec)) {
            std::error_code each;
            const std::string name = entry.path().filename().string();
            if (entry.is_directory(each) && !name.empty() && name.front() != '.') {
                entries.push_back(name);
            }
        }
        std::sort(entries.begin(), entries.end(),
                  [](const json& a, const json& b) {
                      return a.get<std::string>() < b.get<std::string>();
                  });
        return ok(id, json{{"path",   at.string()},
                           {"parent", at.has_parent_path() ? at.parent_path().string() : ""},
                           {"entries", entries}}).dump();
    }

    if (method == "edit.approve") {
        deps_.engine->approve_edit(params.value("approved", false));
        return ok(id, json::object()).dump();
    }

    // Listed by methods() and not handled above. Not a caller's mistake --
    // ours -- and worth saying differently so it is not mistaken for a typo.
    return fail(id, "'" + method + "' is listed but not implemented").dump();
}

}  // namespace crucible::api
