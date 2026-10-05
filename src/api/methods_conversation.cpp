// SPDX-License-Identifier: MIT
//
// Talking to an expert, setting one to work, and what has been said before.
#include "methods.hpp"

#include "crucible/cook/journal.hpp"
#include "crucible/session/store.hpp"

namespace crucible::api {
namespace {

// ---------------------------------------------------------------------------
// A turn
// ---------------------------------------------------------------------------

Reply snapshot(const json&, Host& host) {
    if (host.state() == nullptr) {
        return no_engine();
    }
    // Parsed back rather than assembled a second time, so the snapshot a
    // caller asks for and the one the window is pushed cannot differ.
    json out = json::parse(Surface(host).snapshot(), nullptr, false);
    return out.is_object() ? good(std::move(out)) : bad("the state could not be read");
}

Reply submit(const json& params, Host& host) {
    if (host.engine() == nullptr) {
        return no_engine();
    }
    const auto prompt = params.value("prompt", std::string{});
    if (prompt.empty()) {
        return bad("submit needs a prompt");
    }
    // An expert named here skips routing, which is what `/physics ...` does.
    // An empty one is not an error: it is the normal case, and it means "let
    // the delegator decide".
    const auto expert = params.value("expert", std::string{});
    host.engine()->submit(prompt, expert.empty() ? std::nullopt
                                                 : std::optional<ExpertId>(expert));
    return good();
}

Reply cancel(const json&, Host& host) {
    if (host.engine() == nullptr) {
        return no_engine();
    }
    host.engine()->cancel();
    return good();
}

Reply release(const json& params, Host& host) {
    if (host.engine() == nullptr) {
        return no_engine();
    }
    if (params.value("all", false)) {
        host.engine()->release_all();
    } else {
        host.engine()->release_expert();
    }
    return good();
}

/// Ask it again, or take it away. Both are refused while something is
/// running, by the session rather than here: the buttons are not offered
/// then either, and this is the belt to that brace.
Reply change_turn(const json& params, Host& host, bool retry) {
    if (host.state() == nullptr) {
        return no_engine();
    }
    if (!params.contains("index") || !params["index"].is_number_unsigned()) {
        return bad("which turn?");
    }
    const auto index = params["index"].get<std::size_t>();
    if (index >= host.state()->snapshot().turns.size()) {
        return bad("there is no turn there");
    }
    if (!(retry ? host.retry_turn(index) : host.delete_turn(index))) {
        return bad("the transcript cannot be changed while something is running");
    }
    // Deleting changes the transcript and nothing else: no token arrives to
    // wake the screen afterwards, so the turn would stay visible until
    // something unrelated happened. Retrying wakes on its own, and waking
    // twice costs one redraw.
    host.wake();
    return good(json{{"index", index}});
}

Reply turn_retry(const json& params, Host& host)  { return change_turn(params, host, true); }
Reply turn_delete(const json& params, Host& host) { return change_turn(params, host, false); }

Reply edit_approve(const json& params, Host& host) {
    if (host.engine() == nullptr) {
        return no_engine();
    }
    host.engine()->approve_edit(params.value("approved", false));
    return good();
}

// ---------------------------------------------------------------------------
// A cook
// ---------------------------------------------------------------------------

Reply cook_start(const json& params, Host& host) {
    if (host.engine() == nullptr) {
        return no_engine();
    }
    const auto goal = params.value("goal", std::string{});
    if (goal.empty()) {
        return bad("cook.start needs a goal");
    }
    // A cook reads and writes files, so it needs somewhere it is allowed to.
    // Refused here rather than in each interface, because "which folder has
    // been trusted" is the session's answer and not theirs.
    const std::filesystem::path root = host.project_root();
    if (root.empty()) {
        return bad("no project is open, and a cook works on a project");
    }
    host.engine()->start_cook(goal, params.value("seconds", 0), root);
    return good();
}

Reply cook_stop(const json&, Host& host) {
    if (host.engine() == nullptr) {
        return no_engine();
    }
    // Not a cancel: the cook stops taking new work and makes a finishing
    // pass. The distinction is the whole reason stop_cook exists.
    host.engine()->stop_cook();
    return good();
}

Reply cook_answer(const json& params, Host& host) {
    if (host.engine() == nullptr) {
        return no_engine();
    }
    host.engine()->answer_cook(params.value("answer", std::string{}));
    return good();
}

// ---------------------------------------------------------------------------
// What has been done before
// ---------------------------------------------------------------------------

Reply history(const json&, const Scene& scene) {
    if (scene.project.empty()) {
        return bad("no project is open, and history is per project");
    }
    const Project project = Project::at(scene.project);

    json sessions = json::array();
    for (const SessionSummary& one : SessionStore(project).list()) {
        sessions.push_back(json{{"id", one.id}, {"title", one.title},
                                {"when", one.when()}, {"turns", one.turns}});
    }
    json cooks = json::array();
    for (const CookSummary& one : CookLog(project.dir).list()) {
        cooks.push_back(json{{"id", one.id}, {"goal", one.goal},
                             {"state", std::string(cook_state_name(one.state))},
                             {"when", one.when()},
                             {"files", one.files},
                             {"steps", one.steps},
                             {"seconds", static_cast<long long>(one.duration.count())}});
    }
    return good(json{{"sessions", sessions}, {"cooks", cooks}});
}

/// One cook's journal, whole. Read rather than reopened: a finished cook is a
/// record of what was done, and there is nothing in it to resume.
Reply history_cook(const json& params, const Scene& scene) {
    const auto what = params.value("id", std::string{});
    if (what.empty()) {
        return bad("history.cook needs an id");
    }
    if (scene.project.empty()) {
        return bad("no project is open");
    }
    const std::optional<Cook> cook = CookLog(Project::at(scene.project).dir).load(what);
    if (!cook) {
        return bad("no cook called " + what);
    }
    json steps = json::array();
    for (const CookStep& step : cook->steps) {
        steps.push_back(cook_step_json(step));
    }
    return good(json{{"goal", cook->goal}, {"outcome", cook->outcome},
                     {"state", std::string(cook_state_name(cook->state))},
                     {"headline", cook->headline()},
                     {"files", cook->files_touched()},
                     {"experts", cook->experts_used()},
                     {"seconds", static_cast<long long>(cook->duration().count())},
                     {"steps", std::move(steps)}});
}

Reply history_open(const json& params, Host& host) {
    const auto what = params.value("id", std::string{});
    if (what.empty()) {
        return bad("history.open needs an id");
    }
    const std::string error = host.open_session(what);
    return error.empty() ? good() : bad(error);
}

}  // namespace

void conversation_methods(std::vector<Method>& table) {
    table.push_back({"snapshot",     nullptr, snapshot});
    table.push_back({"submit",       nullptr, submit});
    table.push_back({"cancel",       nullptr, cancel});
    table.push_back({"release",      nullptr, release});
    table.push_back({"turn.retry",   nullptr, turn_retry});
    table.push_back({"turn.delete",  nullptr, turn_delete});
    table.push_back({"edit.approve", nullptr, edit_approve});
    table.push_back({"cook.start",   nullptr, cook_start});
    table.push_back({"cook.stop",    nullptr, cook_stop});
    table.push_back({"cook.answer",  nullptr, cook_answer});
    table.push_back({"history",      history, nullptr});
    table.push_back({"history.cook", history_cook, nullptr});
    table.push_back({"history.open", nullptr, history_open});
}

}  // namespace crucible::api
