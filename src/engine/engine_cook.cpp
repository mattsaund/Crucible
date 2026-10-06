// SPDX-License-Identifier: MIT
//
// The cook: the core's half of it.
//
// The loop itself -- passes until stopped, DONE closing a piece of work,
// HANDOFF going back through the delegator, the finishing pass, noticing a
// cook going in circles -- is the orchestrator's, in Python: see
// scripts/orchestrator/crucible_orchestrator/cook.py. What is here is what it
// asks the core for: an expert loaded into the seat and asked one round, the
// tools that touch the trusted folder, a question to the user about an edit or
// an answer to the cook's own question, and the journal the window draws and
// History reads, saved after every step so a cook killed at minute fifty can
// still say what it changed.
#include "crucible/engine/engine.hpp"

#include <chrono>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <utility>

#include "crucible/config/paths.hpp"
#include "crucible/llm/response_filter.hpp"
#include "crucible/tools/workshop.hpp"
#include "crucible/util/format.hpp"

namespace crucible {
namespace {

using Clock = std::chrono::steady_clock;
using json  = nlohmann::json;

long ms_between(Clock::time_point start) {
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

tools::ToolKind tool_kind_from_name(const std::string& name) {
    for (const tools::ToolKind kind :
         {tools::ToolKind::List, tools::ToolKind::Read, tools::ToolKind::Write, tools::ToolKind::Run,
          tools::ToolKind::Search, tools::ToolKind::Ask, tools::ToolKind::Note, tools::ToolKind::Done,
          tools::ToolKind::Handoff}) {
        if (tools::tool_kind_name(kind) == name) {
            return kind;
        }
    }
    return tools::ToolKind::None;
}

tools::ToolCall call_from(const json& params) {
    tools::ToolCall call;
    call.kind     = tool_kind_from_name(params.value("kind", ""));
    call.argument = params.value("argument", "");
    call.content  = params.value("content", "");
    return call;
}

}  // namespace

// ---------------------------------------------------------------------------
// Queueing and control
// ---------------------------------------------------------------------------

void Engine::set_project(std::filesystem::path root, std::filesystem::path project_dir) {
    project_root_ = std::move(root);
    cook_log_     = std::make_unique<CookLog>(std::move(project_dir));
}

tools::WorkshopSettings Engine::workshop_for(const std::filesystem::path& root) const {
    tools::WorkshopSettings workshop;
    // Having a root is the permission. It is only ever set to a folder the user
    // answered the trust question for, and that question is asked in the terms
    // this is actually about: read, write, run, inside this directory.
    workshop.enabled             = !root.empty();
    workshop.root                = root;
    workshop.allow_run           = !root.empty();
    workshop.run_timeout_seconds = config_.tools.workshop_timeout;
    return workshop;
}

void Engine::start_cook(std::string goal, int budget_seconds, std::filesystem::path root,
                        std::vector<attach::Attachment> attachments,
                        std::optional<ExpertId> pinned) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        Request request;
        request.kind           = RequestKind::Cook;
        request.prompt         = std::move(goal);
        request.budget_seconds = budget_seconds;
        request.root           = std::move(root);
        request.attachments    = std::move(attachments);
        request.pinned         = std::move(pinned);
        pending_.push_back(std::move(request));
    }
    queued_.notify_one();
}

void Engine::stop_cook() {
    cook_stop_.store(true, std::memory_order_relaxed);
    // A cook parked on a question is inside await_cook_answer, not looking at
    // the flag. Waking it is what lets /stop end a cook that is waiting on you.
    cook_answered_.notify_all();
}

void Engine::answer_cook(std::string answer) {
    {
        const std::lock_guard<std::mutex> lock(cook_answer_mutex_);
        cook_answer_ = std::move(answer);
    }
    cook_answered_.notify_all();
}

std::optional<std::string> Engine::await_cook_answer() {
    std::unique_lock<std::mutex> lock(cook_answer_mutex_);
    cook_answered_.wait(lock, [this] {
        return cook_answer_.has_value() || cook_stop_.load(std::memory_order_relaxed)
            || !running_.load(std::memory_order_relaxed);
    });
    return std::exchange(cook_answer_, std::nullopt);
}

void Engine::publish_cook() {
    state_.set_cook(std::make_shared<const Cook>(cook_));
    if (wake_) {
        wake_();
    }
}

// ---------------------------------------------------------------------------
// One round with the expert
// ---------------------------------------------------------------------------

Engine::CookRound Engine::cook_round(ChatModel& model, const ModelParams& params,
                                     const std::vector<ChatMessage>& messages) {
    CookRound round;
    const auto start = Clock::now();

    // The same filter the ordinary turn path uses, so a reasoning model's
    // channel markers do not end up being parsed as tool calls.
    ResponseFilter filter;
    const CancelCallback cancel = [this] {
        return cancel_.load(std::memory_order_relaxed)
            || !running_.load(std::memory_order_relaxed);
    };

    ChatSink sink;
    sink.cancel = cancel;
    if (model.reasons_inline()) {
        sink.on_text = [&](std::string_view chunk) {
            const ResponseFilter::Piece piece = filter.feed(chunk);
            round.answer    += piece.answer;
            round.reasoning += piece.reasoning;
        };
    } else {
        sink.on_text = [&](std::string_view chunk) { round.answer += chunk; };
    }
    sink.on_reasoning = [&](std::string_view chunk) { round.reasoning += chunk; };

    // No effort for a remote expert's sake that a local one would not get: a
    // cook's system prompt does not carry the "Reasoning:" line either, since
    // an hour of rounds is not where anybody wants the most thinking per round.
    const ChatResult outcome = model.chat(ChatRequest{messages, params, {}}, sink);

    const ResponseFilter::Piece tail = filter.flush();
    round.answer    += tail.answer;
    round.reasoning += tail.reasoning;
    round.error = outcome.error;
    if (outcome.discard_partial) {
        round.answer.clear();
    }
    round.ms = ms_between(start);
    return round;
}

// ---------------------------------------------------------------------------
// What the orchestrator asks for while it cooks
// ---------------------------------------------------------------------------

nlohmann::json Engine::serve_cook(const std::string& method, const nlohmann::json& params) {
    // --- the seat ------------------------------------------------------------
    if (method == "seat.take") {
        const ExpertId    id   = params.value("expert", "");
        const std::string name = params.value("name", expert_label(config_.roster, id));
        if (!config_.has_expert(id)) {
            cook_seat_ = CookSeat{};
            return json{{"ok", false}, {"error", name + " has no model"}};
        }
        const ModelParams seat_params = config_.expert(id);
        // The delegator has done its routing, and an expert on this machine is
        // about to want every byte it was holding. See resolve().
        if (!config_.routing.keep_delegator_loaded && !seat_params.remote()) {
            release_router();
        }
        // The common case, and the one that must not cost a reload: the
        // delegator chose whoever is already in the chair.
        if (cook_seat_.model != nullptr && cook_seat_.id == id) {
            return json{{"ok", true}, {"load_ms", 0}};
        }
        state_.set_linked(id);
        if (wake_) {
            wake_();
        }
        long        load_ms = 0;
        std::string error;
        ChatModel*  model = seat_model(id, seat_params, name, load_ms, error);
        if (model == nullptr) {
            // Whoever was in the seat was freed to make room, so there is
            // nobody in it now. Taking it again loads them again.
            cook_seat_ = CookSeat{};
            return json{{"ok", false}, {"error", error.empty() ? name + " could not be loaded" : error}};
        }
        cook_seat_  = CookSeat{id, name, seat_params, model};
        cook_images_.clear();
        return json{{"ok", true}, {"load_ms", load_ms}};
    }
    if (method == "seat.chat") {
        if (cook_seat_.model == nullptr) {
            return json{{"error", "no expert is in the seat"}, {"ms", 0}};
        }
        std::vector<ChatMessage> messages;
        for (const json& one : params.value("messages", json::array())) {
            ChatMessage message{one.value("role", "user"), one.value("content", "")};
            if (one.value("attached", false)) {
                message.images = cook_images_;
            }
            messages.push_back(std::move(message));
        }
        const CookRound round = cook_round(*cook_seat_.model, cook_seat_.params, messages);
        return json{{"answer", round.answer}, {"reasoning", round.reasoning},
                    {"error", round.error}, {"ms", round.ms}};
    }
    if (method == "seat.attachments") {
        if (cook_seat_.model == nullptr || cook_attachments_.empty()) {
            return json{{"text", ""}, {"images", 0}};
        }
        // Read for this seat: its context is the size the share is of, and
        // whether it sees pictures is its own.
        attach::Composed composed =
            read_attachments(cook_attachments_, *cook_seat_.model,
                             {{"system", params.value("system", "")}}, params.value("share", 0.33));
        cook_images_.clear();
        for (attach::Image& image : composed.images) {
            cook_images_.push_back({std::move(image.mime), std::move(image.data)});
        }
        return json{{"text", composed.text}, {"images", cook_images_.size()}};
    }

    // --- the tools -------------------------------------------------------------
    if (method == "tools.parse") {
        const std::string answer    = params.value("answer", "");
        const std::string reasoning = params.value("reasoning", "");
        const std::optional<tools::ToolCall> call = tools::parse_tool_call(answer, reasoning);
        if (!call) {
            return json{{"call", nullptr},
                        {"attempted", std::string(tools::tool_kind_name(
                                          tools::attempted_tool_call(answer, reasoning)))}};
        }
        return json{{"call", {{"kind", std::string(tools::tool_kind_name(call->kind))},
                              {"argument", call->argument},
                              {"content", call->content}}},
                    {"attempted", "none"}};
    }
    if (method == "tools.run") {
        const tools::ToolCall call = call_from(params);
        if (call.kind == tools::ToolKind::None) {
            throw std::runtime_error("no such tool: " + params.value("kind", std::string()));
        }
        const tools::ToolResult result = tools::run_tool(call, cook_workshop_, cook_search_,
            [this] { return cancel_.load(std::memory_order_relaxed); });
        return json{{"ok", result.ok}, {"output", result.output}, {"summary", result.summary},
                    {"detail", result.detail}, {"changed", result.changed}};
    }
    if (method == "edit.ask") {
        tools::ToolCall call = call_from(params);
        call.kind = tools::ToolKind::Write;
        state_.set_mood(Mood::Idle, "waiting on you: " + call.argument);
        return json{{"approved", ask_about_edit(call, cook_workshop_)}};
    }

    // --- the journal and the person ---------------------------------------
    if (method == "cook.publish") {
        // The orchestrator owns the cook as it runs; this is where the window
        // and History see it. Saved after every step rather than at the end.
        cook_ = cook_from_json(params.value("cook", json::object()), cook_.id);
        publish_cook();
        if (cook_log_) {
            std::string error;
            cook_log_->save(cook_, error);
        }
        return json::object();
    }
    if (method == "cook.await_answer") {
        const std::optional<std::string> answer = await_cook_answer();
        return json{{"answer", answer ? json(*answer) : json(nullptr)}};
    }

    throw std::runtime_error("the core has no method " + method);
}

// ---------------------------------------------------------------------------
// A cook
// ---------------------------------------------------------------------------

void Engine::do_cook(const std::string& goal, int budget_seconds,
                     const std::filesystem::path& root,
                     std::vector<attach::Attachment> attachments,
                     std::optional<ExpertId> pinned) {
    cooking_.store(true, std::memory_order_relaxed);
    cook_stop_.store(false, std::memory_order_relaxed);
    cancel_.store(false, std::memory_order_relaxed);
    {
        const std::lock_guard<std::mutex> lock(cook_answer_mutex_);
        cook_answer_.reset();
    }

    cook_                = Cook{};
    cook_.id             = CookLog::new_id();
    cook_.goal           = goal;
    cook_.state          = CookState::Working;
    cook_.budget_seconds = budget_seconds;
    cook_.started_unix   = static_cast<std::int64_t>(std::time(nullptr));
    for (const attach::Attachment& one : attachments) {
        cook_.attachments.push_back(attach::tile_of(one));
    }
    publish_cook();

    cook_workshop_ = workshop_for(root);
    cook_search_   = tools::SearchSettings{};
    cook_search_.enabled         = config_.tools.web_search;
    cook_search_.provider        = config_.tools.search_provider;
    cook_search_.endpoint        = config_.tools.search_endpoint;
    cook_search_.api_key         = config_.tools.search_api_key;
    cook_search_.max_results     = config_.tools.search_results;
    cook_search_.timeout_seconds = config_.tools.search_timeout;
    cook_attachments_ = std::move(attachments);
    cook_images_.clear();
    cook_seat_ = CookSeat{};

    json request = routing_request(goal, pinned);
    const json journal = cook_to_json(cook_);
    request["id"]              = cook_.id;
    request["goal"]            = goal;
    request["root"]            = root.string();
    request["budget_seconds"]  = budget_seconds;
    request["attachments"]     = journal["attachments"];
    request["has_attachments"] = !cook_attachments_.empty();
    request["system_prompt"]   = config_.system_prompt;
    request["tools"]           = tools::workshop_instructions(cook_workshop_, tools::ToolAudience::Cook);

    std::string error;
    if (wait_for_python(error)) {
        try {
            orchestra_.call("cook.run", request,
                            [this](const std::string& method, const json& params) {
                                return serve(method, params);
                            });
        } catch (const std::exception& e) {
            error = std::string("the cook stopped: ") + e.what();
        }
    }

    // However it ended, the journal is finished and kept.
    if (!error.empty() || cook_.state == CookState::Working || cook_.state == CookState::Asking
        || cook_.state == CookState::Finishing) {
        cook_.state = error == "stopped" || cancel_.load(std::memory_order_relaxed)
                          ? CookState::Stopped : CookState::Failed;
        if (!error.empty() && error != "stopped") {
            cook_.outcome = error;
        } else if (cook_.outcome.empty()) {
            cook_.outcome = "interrupted";
        }
    }
    if (cook_.ended_unix == 0) {
        cook_.ended_unix = static_cast<std::int64_t>(std::time(nullptr));
    }
    publish_cook();
    if (cook_log_) {
        std::string save_error;
        cook_log_->save(cook_, save_error);
    }

    state_.set_linked(std::nullopt);
    if (cook_.state == CookState::Failed && cook_.steps.empty()) {
        state_.set_mood(Mood::Error, cook_.outcome);   // it never started
    } else {
        state_.set_mood(Mood::Idle, "cook finished -- " + cook_.headline());
    }
    cook_seat_ = CookSeat{};
    cook_images_.clear();
    cook_attachments_.clear();
    cooking_.store(false, std::memory_order_relaxed);
    cook_stop_.store(false, std::memory_order_relaxed);
    if (wake_) {
        wake_();
    }
}

}  // namespace crucible
