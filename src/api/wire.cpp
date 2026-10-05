// SPDX-License-Identifier: MIT
//
// The engine's state, as an interface reads it.
//
// One function's worth of file, and kept apart from the methods because it is
// the one shape that crosses without being asked for: the window pushes it to
// the page every time the engine wakes it.
#include "methods.hpp"

#include "crucible/cook/journal.hpp"

namespace crucible::api {
namespace {

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

/// Where a routing decision came from, as the transcript says it.
std::string_view route_source_word(RouteSource source) {
    switch (source) {
        case RouteSource::Model:    return "router model";
        case RouteSource::Keyword:  return "keywords";
        case RouteSource::Fallback: return "fallback";
        case RouteSource::Forced:   return "pinned";
    }
    return "fallback";
}

json turn_json(const Turn& turn) {
    json out{
        {"prompt",            turn.prompt},
        {"reply",             turn.reply},
        {"streaming",         turn.streaming},
        {"canceled",          turn.canceled},
        {"failed",            turn.failed},
        {"tokens_per_second", turn.tokens_per_second},
        {"prompt_tokens",     turn.prompt_tokens},
        {"output_tokens",     turn.output_tokens},
        {"load_ms",           turn.load_ms},
    };
    // Reasoning is sent but kept apart from the reply, for the same reason the
    // window draws it apart: it is not the answer, and an interface that
    // concatenated them would make a reasoning model look broken.
    if (!turn.reasoning.empty()) {
        out["reasoning"] = turn.reasoning;
    }
    if (!turn.attachments.empty()) {
        out["attachments"] = attachments_json(turn.attachments);
    }
    if (turn.route) {
        // How the decision was reached, not just what it was. "the delegator
        // chose this, 100%" and "nothing chose it, this is the fallback" are
        // different claims and the screen should not make them look alike.
        out["route"] = json{
            {"expert",     turn.route->expert},
            {"confidence", turn.route->confidence},
            {"source",     route_source_word(turn.route->source)},
            {"detail",     turn.route->detail},
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

}  // namespace

json attachments_json(const std::vector<attach::Tile>& attachments) {
    json out = json::array();
    for (const TurnAttachment& one : attachments) {
        out.push_back(json{{"path", one.path}, {"name", one.name}, {"label", one.label}, {"kind", one.kind}});
    }
    return out;
}

json cook_step_json(const CookStep& step) {
    json one{{"iteration", step.iteration},
             {"expert",    step.expert},
             {"kind",      step.kind},
             {"summary",   step.summary},
             {"ok",        step.ok},
             {"ms",        step.ms}};
    if (!step.detail.empty())  { one["detail"]  = step.detail; }
    if (!step.changed.empty()) { one["changed"] = step.changed; }
    return one;
}

/// The whole drawable state.
///
/// Sent entire rather than as a diff. A snapshot is a few kilobytes, it is
/// produced a few times a second rather than per token, and a diff protocol
/// would buy some bandwidth across an in-process call in exchange for two
/// sides that can disagree about what they are looking at.
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

    // Present only while it is loading, so "is the delegator loading" and "how
    // far along" cannot disagree.
    if (snapshot.delegator_progress >= 0.0F) {
        out["delegator_progress"] = snapshot.delegator_progress;
    }
    if (snapshot.resident) { out["resident"] = *snapshot.resident; }
    if (snapshot.linked)   { out["linked"]   = *snapshot.linked; }

    // The roster and the seat states are parallel arrays in the engine and one
    // array of objects here. Two arrays that have to be zipped by index is a
    // thing every interface would have to get right separately.
    json experts = json::array();
    if (snapshot.roster) {
        const std::vector<Expert>& list = snapshot.roster->experts();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const bool seated = i < snapshot.seats.size();
            json one{
                {"id",    list[i].id},
                {"name",  list[i].name},
                {"tag",   list[i].tag},
                {"blurb", list[i].blurb},
                {"phase", seat_word(seated ? snapshot.seats[i].phase
                                           : SeatPhase::Unconfigured)},
                {"progress", seated ? snapshot.seats[i].progress : 0.0F},
            };
            // Said only for the seats it is true of. A prompt routed to one
            // of these leaves the machine, and the panel marks it.
            if (seated && !snapshot.seats[i].provider.empty()) {
                one["provider"] = snapshot.seats[i].provider;
            }
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
        const Cook& cook = *snapshot.cook;
        json steps = json::array();
        // The tail rather than all of them. An hour's cook is thousands of
        // steps, the view shows the end of the list, and sending the whole
        // journal on every wake would be most of the bytes for none of the
        // information. The full record is on disk and history reads it.
        const std::size_t keep = 200;
        const std::size_t from = cook.steps.size() > keep ? cook.steps.size() - keep : 0;
        for (std::size_t i = from; i < cook.steps.size(); ++i) {
            steps.push_back(cook_step_json(cook.steps[i]));
        }
        out["cook"] = json{
            {"running",  cook.state == CookState::Working || cook.state == CookState::Asking
                             || cook.state == CookState::Finishing},
            {"id",       cook.id},
            {"goal",     cook.goal},
            {"attachments", attachments_json(cook.attachments)},
            {"state",    std::string(cook_state_name(cook.state))},
            {"question", cook.question},
            {"outcome",  cook.outcome},
            {"headline", cook.headline()},
            {"iterations", cook.iterations},
            {"started",  cook.started_unix},
            {"ended",    cook.ended_unix},
            {"seconds",  static_cast<long long>(cook.duration().count())},
            {"experts",  cook.experts_used()},
            {"files",    cook.files_touched()},
            {"total",    cook.steps.size()},
            {"shown_from", from},
            {"steps",    std::move(steps)},
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

std::string snapshot_json(const Snapshot& snapshot) {
    return snapshot_to_json(snapshot).dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace crucible::api
