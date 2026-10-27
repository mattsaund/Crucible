// SPDX-License-Identifier: MIT
//
// A chat turn: the core's half of it.
//
// The turn itself -- who answers, how many rounds it may take, which call the
// expert made and whether to run it, what it is told about the result, when
// it has answered, and what the transcript says when it has not -- is the
// orchestrator's, in Python: see
// scripts/orchestrator/crucible_orchestrator/chat.py. The worker opens the
// turn in the transcript and asks for it, and what is here is what the
// orchestrator asks for while it runs one: the expert seated and its model
// held for the turn, the conversation it belongs to fitted to that model's
// context, each round streamed into the transcript as it is written, the
// tools that touch the trusted folder, a web search, an edit put to the
// person, and the transcript's record of the rest.
//
// Two things stay here that could look like decisions. The streaming does --
// the paragraph between rounds, the status line when the first word arrives,
// the rate -- because only this side sees the tokens, one at a time, and a
// line across the pipe for each would be a cost for nothing to decide. And so
// does fitting the conversation to the context, which counts it with the
// model's own tokenizer over and over as exchanges are dropped; whether to
// drop or refuse is the orchestrator's.
#include "crucible/engine/engine.hpp"

#include <chrono>
#include <map>
#include <stdexcept>

#include "crucible/engine/overflow.hpp"
#include "crucible/llm/response_filter.hpp"
#include "crucible/tools/web_search.hpp"
#include "crucible/tools/workshop.hpp"

namespace crucible {
namespace {

using json  = nlohmann::json;
using Clock = std::chrono::steady_clock;

/// Where to search and how, as `config` says.
tools::SearchSettings search_settings(const Config& config) {
    tools::SearchSettings settings;
    settings.enabled         = config.tools.web_search;
    settings.provider        = config.tools.search_provider;
    settings.endpoint        = config.tools.search_endpoint;
    settings.api_key         = config.tools.search_api_key;
    settings.max_results     = config.tools.search_results;
    settings.timeout_seconds = config.tools.search_timeout;
    return settings;
}

Mood mood_named(const std::string& mood) {
    return mood == "thinking" ? Mood::Thinking
         : mood == "loading"  ? Mood::Loading
         : mood == "routing"  ? Mood::Routing
         : mood == "error"    ? Mood::Error
                              : Mood::Idle;
}

/// Whether `root` has anything in it a person put there: a file or a folder
/// whose name does not start with a dot. The first one found is enough.
bool has_files(const std::filesystem::path& root) {
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(root, ec)) {
        return false;
    }
    for (std::filesystem::directory_iterator it{root, ec}, end; it != end && !ec; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (!name.empty() && name.front() != '.') {
            return true;
        }
    }
    return false;
}

}  // namespace

/// What the core keeps of the chat turn being answered, between the calls the
/// orchestrator makes of it.
struct Engine::ChatTurn {
    std::size_t                     index = 0;   ///< the turn, in the transcript
    std::string                     prompt;
    std::vector<attach::Attachment> attachments;
    std::optional<ExpertId>         pinned;      ///< sent to one seat, for a build it becomes

    /// The settings as they are when the turn starts, kept for the whole of
    /// it: a build running beside it can add a seat to the engine's own copy
    /// at any moment, and a turn read halfway through a change is a turn
    /// answered by half of two configurations.
    Config config;

    // Who answers, once the turn is routed and the expert seated.
    ExpertId                expert;
    bool                    routed = false;
    std::string             name;      ///< the expert's name, for the status line
    ModelParams             params;
    HeldModel               held;      ///< the model, held until the turn is over
    long                    load_ms = 0;
    tools::WorkshopSettings workshop;

    /// The conversation as the turn opened it: the instructions, the exchanges
    /// before this one, and the question, fitted to the context. A round is
    /// this and the turn's own exchanges, which the orchestrator sends.
    std::vector<ChatMessage> opening;

    /// The question as the expert read it, attachments and all: what the
    /// history keeps of this turn.
    ChatMessage asked;

    /// The pictures the turn's tools produced, by the name the orchestrator
    /// hands back with the message they go with. A screenshot goes with every
    /// round after the one that took it, in the place it was handed back.
    std::map<std::string, std::vector<ChatImage>> pictures;

    // How the answer is arriving, across all the turn's rounds: the status
    // line says the expert is thinking at the turn's first token, and the
    // live rate is measured from it -- everything before is reading the
    // prompt, and folding that in would make a long prompt look like a slow
    // expert.
    bool              first_token = true;
    Clock::time_point first_token_at;
    int               streamed_chunks = 0;

    /// What the rounds cost, for the turn's record.
    GenerationStats stats;

    /// The orchestrator closed the turn. One it did not is closed by the
    /// worker, failed, when the orchestrator is done with it.
    bool ended = false;

    std::mutex mutex;   ///< one call at a time
};

void Engine::handle(const Request& request) {
    std::vector<TurnAttachment> tiles;
    for (const attach::Attachment& one : request.attachments) {
        tiles.push_back(attach::tile_of(one));
    }
    const std::size_t turn = state_.begin_turn(request.prompt, std::move(tiles));

    auto chat         = std::make_shared<ChatTurn>();
    chat->index       = turn;
    chat->prompt      = request.prompt;
    chat->attachments = request.attachments;
    chat->pinned      = request.pinned;
    chat->config      = this->config();

    state_.set_mood(Mood::Routing, "Crucible is reading the prompt");
    if (wake_) {
        wake_();
    }

    std::string error;
    if (!wait_for_python(error)) {
        if (error == "stopped") {
            state_.cancel_turn(turn);
            state_.set_mood(Mood::Idle);
        } else {
            state_.fail_turn(turn, error);
            state_.set_mood(Mood::Error, error);
        }
        return;
    }

    // What the orchestrator needs for the whole turn: how to route it, and
    // what each choice it makes along the way turns on.
    json asked = routing_request(chat->config, request.prompt, request.pinned);
    json names = json::array();
    for (const attach::Attachment& one : request.attachments) {
        names.push_back(one.name.empty() ? std::filesystem::path(one.path).filename().string() : one.name);
    }
    asked["context"]       = "chat";
    asked["turn"]          = turn;
    asked["attachments"]   = names;
    asked["system_prompt"] = chat->config.system_prompt;
    asked["effort"]        = chat->config.reasoning_effort;
    asked["web_search"]    = chat->config.tools.web_search;
    asked["search_rounds"] = chat->config.tools.search_rounds;
    asked["search_tools"]  = tools::tool_instructions();
    asked["overflow"]      = chat->config.tools.overflow;
    // Whether this message may become a build (see the orchestrator's
    // intent.py), and whether the folder it would be built in has anything
    // in it yet. Not while a build runs: one at a time, and a message typed
    // beside one is answered. Nor one sent to be answered whatever it asks.
    asked["can_build"] = !request.answer_only && !cooking() && !project_root_.empty();
    asked["files"]     = has_files(project_root_);

    {
        const std::lock_guard<std::mutex> lock(chat_mutex_);
        chat_turn_ = chat;
    }
    try {
        orchestra_.call("chat.turn", asked);
    } catch (const std::exception& e) {
        error = e.what();
    }
    {
        const std::lock_guard<std::mutex> lock(chat_mutex_);
        chat_turn_.reset();
    }

    // A turn the orchestrator did not close -- it went away, or could not
    // be started -- is closed here, failed, with what was said about why:
    // left open, it would read as an answer that is still coming.
    const std::lock_guard<std::mutex> one_call(chat->mutex);
    if (!chat->ended) {
        const std::string why = error.empty() ? std::string("the turn ended without an answer")
                              : chat->routed  ? error
                                              : "routing failed: " + error;
        state_.fail_turn(turn, why);
        state_.set_linked(std::nullopt);
        state_.set_mood(Mood::Error, why);
        chat->ended = true;
    }
    // The lease on the model goes with `chat`, as this returns. The table is
    // put back by settle(), once the engine is no longer busy.
}

json Engine::serve_turn(const std::string& method, const json& params) {
    std::shared_ptr<ChatTurn> chat;
    {
        const std::lock_guard<std::mutex> lock(chat_mutex_);
        chat = chat_turn_;
    }
    if (!chat || params.value("turn", std::int64_t{-1}) != static_cast<std::int64_t>(chat->index)) {
        throw std::runtime_error("no chat turn is open by that number");
    }
    const std::lock_guard<std::mutex> one_call(chat->mutex);
    if (chat->ended) {
        throw std::runtime_error("the chat turn is over");
    }
    const std::size_t    turn   = chat->index;
    const CancelCallback cancel = [this] {
        return cancel_.load(std::memory_order_relaxed) || !running_.load(std::memory_order_relaxed);
    };

    // --- or a build ------------------------------------------------------------
    if (method == "turn.build") {
        // The message is a build: started now, on its own words and what came
        // with them, in the folder this turn is about, and sent to the seat
        // it was sent to when it was. The turn is the build's from here (see
        // Turn::build), and over for the chat; the build's end is its reply.
        std::string id;
        const std::string refused =
            start_cook(chat->prompt, 0, project_root_, chat->attachments, chat->pinned, "build", &id);
        if (!refused.empty()) {
            return json{{"ok", false}, {"error", refused}};
        }
        state_.set_build(turn, id);
        if (wake_) {
            wake_();
        }
        return json{{"ok", true}, {"id", id}};
    }

    // --- who answers ---------------------------------------------------------
    if (method == "turn.route") {
        const json decided = params.value("decision", json::object());
        RouteDecision decision;
        decision.expert     = decided.value("expert", "");
        decision.confidence = decided.value("confidence", 0.0F);
        decision.source     = route_source_from_name(decided.value("source", "fallback"));
        decision.detail     = decided.value("detail", "");
        if (!chat->config.routing.keep_delegator_loaded && !chat->config.expert(decision.expert).remote()) {
            // Its work for this prompt is done -- or there was none, for a
            // prompt sent to one expert -- and the expert is about to want
            // every byte it was holding. Unless the expert is somewhere else:
            // then nothing is about to be loaded, and freeing the delegator
            // would only mean loading it again for the next prompt.
            release_router_between_routes();
        }
        chat->expert = decision.expert;
        chat->routed = true;
        state_.set_route(turn, decision);
        // From here the side menu draws a line from the delegator to this seat.
        state_.set_linked(decision.expert);
        if (wake_) {
            wake_();
        }
        return json{{"has_model", chat->config.has_expert(decision.expert)}};
    }
    if (method == "turn.load") {
        // The display name, resolved once: every status line wants it, and a
        // seat ejected mid-turn would otherwise make each of them fall back
        // to the raw id on its own.
        chat->name   = expert_label(chat->config.roster, chat->expert);
        chat->params = chat->config.expert(chat->expert);
        std::string error;
        // Held for the whole turn: the lease is what keeps a build's thread
        // from freeing this model while the turn is answering with it.
        chat->held = hold_model(chat->expert, chat->params, chat->name, cancel, chat->load_ms, error);
        if (chat->held.model == nullptr) {
            return json{{"ok", false}, {"error", error}};
        }
        const ChatModel& model = *chat->held.model;
        // The tools a cook gets, in a folder the person has trusted. An empty
        // root is a workshop with nothing in it to offer.
        chat->workshop               = workshop_for(project_root_);
        chat->workshop.sees_pictures = model.sees_images();
        return json{{"ok", true}, {"remote", chat->params.remote()},
                    {"sees_images", model.sees_images()}, {"takes_effort", model.takes_effort()},
                    {"workshop", chat->workshop.enabled},
                    {"tools", tools::workshop_instructions(chat->workshop, tools::ToolAudience::Chat)}};
    }

    if (chat->held.model == nullptr && method != "turn.end" && method != "turn.show") {
        throw std::runtime_error("the turn's expert is not seated");
    }

    // --- the conversation ----------------------------------------------------
    if (method == "turn.open") {
        ChatModel& model = *chat->held.model;
        std::vector<ChatMessage> messages{{"system", params.value("system", std::string())}};
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            const std::size_t keep = params.value("history", std::size_t{0});
            const std::size_t from = history_.size() > keep ? history_.size() - keep : 0;
            messages.insert(messages.end(), history_.begin() + static_cast<long>(from), history_.end());
        }
        ChatMessage asked{"user", chat->prompt};
        if (!chat->attachments.empty()) {
            // Read for this model: how much of a long document fits is a
            // question about its context, less what the conversation takes.
            messages.push_back(asked);
            attach::Composed composed =
                read_attachments(chat->attachments, model, messages, params.value("share", 0.75));
            messages.pop_back();
            asked.content = composed.text + chat->prompt;
            for (attach::Image& image : composed.images) {
                asked.images.push_back({std::move(image.mime), std::move(image.data)});
            }
        }
        messages.push_back(asked);
        chat->asked   = std::move(asked);
        chat->opening = std::move(messages);
        return json{{"used", model.prompt_tokens(chat->opening)}, {"size", model.context_size()}};
    }
    if (method == "turn.fit") {
        // The expert's own tokenizer through its own chat template, which is
        // the only count that means anything. See overflow.hpp.
        ChatModel& model = *chat->held.model;
        const TokenCounter counter = [&model](const std::vector<ChatMessage>& what) {
            return model.prompt_tokens(what);
        };
        const std::size_t dropped = trim_to_budget(overflow_from_id(params.value("policy", std::string())),
                                                   chat->opening, counter, params.value("budget", 0));
        state_.set_context_used(model.prompt_tokens(chat->opening), model.context_size());
        return json{{"dropped", dropped}};
    }

    // --- one round -------------------------------------------------------------
    if (method == "turn.round") {
        ChatModel& model = *chat->held.model;
        std::vector<ChatMessage> messages = chat->opening;
        for (const json& one : params.value("messages", json::array())) {
            ChatMessage message{one.value("role", "user"), one.value("content", "")};
            if (const auto held = chat->pictures.find(one.value("pictures", std::string()));
                held != chat->pictures.end()) {
                message.images = held->second;
            }
            messages.push_back(std::move(message));
        }
        // A paragraph between what the rounds before this one left on screen
        // and what this one says, rather than one run into the other.
        const bool        paragraph    = params.value("paragraph", false);
        bool              first_answer = true;
        std::string       answer;
        std::string       reasoning;
        ResponseFilter    filter;
        const std::string name = chat->name;

        // The two things a piece of output can be, wherever it came from.
        const auto take = [&](std::string_view thought, std::string_view said) {
            if (chat->first_token) {
                chat->first_token    = false;
                chat->first_token_at = Clock::now();
                state_.set_mood(Mood::Thinking, name + " is thinking");
            }
            if (!thought.empty()) {
                reasoning += thought;
                state_.append_reasoning(turn, thought);
            }
            if (!said.empty()) {
                if (first_answer) {
                    first_answer = false;
                    state_.set_mood(Mood::Talking, name + " is answering");
                    if (paragraph) {
                        state_.append_reply(turn, "\n\n");
                    }
                }
                answer += said;
                state_.append_reply(turn, said);
            }

            // Recomputing the rate on every token would be noise on screen
            // and work in the hot path; a few times a second is what a
            // person can actually read.
            if (++chat->streamed_chunks % 8 == 0) {
                const double elapsed_s = std::chrono::duration<double>(Clock::now() - chat->first_token_at).count();
                if (elapsed_s > 0.05) {
                    state_.set_live_rate(static_cast<double>(chat->streamed_chunks) / elapsed_s);
                }
            }

            if (wake_) {
                wake_();
            }
        };

        ChatSink sink;
        sink.cancel = cancel;
        if (model.reasons_inline()) {
            // A reasoning model writes its working before its answer, and on
            // some of them the markers between the two are ordinary visible
            // text. See llm/response_filter.hpp.
            sink.on_text = [&](std::string_view raw) {
                const ResponseFilter::Piece chunk = filter.feed(raw);
                take(chunk.reasoning, chunk.answer);
            };
        } else {
            sink.on_text = [&](std::string_view said) { take({}, said); };
        }
        // A channel of its own, which is how a provider sends it. Some send
        // both ways at once, and both are reasoning.
        sink.on_reasoning = [&](std::string_view thought) { take(thought, {}); };

        const ChatResult outcome =
            model.chat(ChatRequest{messages, chat->params, chat->config.reasoning_effort}, sink);
        const GenerationStats& pass = outcome.stats;

        // Whatever was still held back, waiting to see if it was a marker.
        if (const ResponseFilter::Piece last = filter.flush(); !last.answer.empty() || !last.reasoning.empty()) {
            state_.append_reasoning(turn, last.reasoning);
            state_.append_reply(turn, last.answer);
            reasoning += last.reasoning;
            answer += last.answer;
        }

        if (outcome.error.empty()) {
            GenerationStats& stats = chat->stats;
            stats.prompt_tokens += pass.prompt_tokens;
            stats.prompt_reused += pass.prompt_reused;
            stats.output_tokens += pass.output_tokens;
            stats.prompt_ms     += pass.prompt_ms;
            stats.output_ms     += pass.output_ms;
            stats.canceled       = pass.canceled;
            stats.hit_limit      = pass.hit_limit;
        }
        const Snapshot now = state_.snapshot();
        return json{{"answer", answer},
                    {"reasoning", reasoning},
                    {"error", outcome.error},
                    {"discard", outcome.discard_partial},
                    {"notes", outcome.notes},
                    {"canceled", pass.canceled},
                    {"hit_limit", pass.hit_limit},
                    {"output_tokens", pass.output_tokens},
                    {"reply", turn < now.turns.size() ? now.turns[turn].reply : std::string()}};
    }

    // --- the tools ---------------------------------------------------------------
    if (method == "turn.ask_edit") {
        tools::ToolCall call = call_from(params.value("call", json::object()));
        // Asked as the file the change would leave: a WRITE's contents, or an
        // EDIT's blocks applied. An EDIT that would not apply is not put to
        // the person -- it fails with its reason when it is run.
        if (call.kind == tools::ToolKind::Edit) {
            std::string why;
            const std::optional<std::string> after = tools::edited_contents(call, chat->workshop, why);
            if (!after) {
                return json{{"approved", true}};
            }
            call = tools::ToolCall{tools::ToolKind::Write, call.argument, *after, {}, {}};
        }
        // The protocol line comes off the transcript before the question goes
        // up, not after it is answered. Left on, the file is on screen twice
        // while the person decides -- once as a raw `WRITE: path` and a fence,
        // once in the panel asking about it -- and the raw one is the worse
        // copy: no header, no line numbers, and a language nobody declared.
        if (tools::resolve_in_root(chat->workshop.root, call.argument)) {
            state_.set_reply(turn, params.value("reply", std::string()));
        }
        return json{{"approved", ask_about_edit(call, chat->workshop, cancel)}};
    }
    if (method == "turn.tool") {
        const tools::ToolCall call = call_from(params.value("call", json::object()));
        if (call.kind == tools::ToolKind::None) {
            throw std::runtime_error("no such tool: " + params.value("call", json::object()).value("kind", std::string()));
        }
        const tools::ToolResult result = tools::run_tool(call, chat->workshop, search_settings(chat->config), cancel);
        // The pictures it made, kept for the rounds that follow under a name
        // the orchestrator hands back with the message they go with.
        std::string pictures;
        if (!result.pictures.empty()) {
            pictures = "p" + std::to_string(chat->pictures.size() + 1);
            std::vector<ChatImage>& kept = chat->pictures[pictures];
            for (const attach::Image& image : result.pictures) {
                kept.push_back({image.mime, image.data});
            }
        }
        return json{{"ok", result.ok}, {"output", result.output}, {"summary", result.summary},
                    {"detail", result.detail}, {"changed", result.changed},
                    {"picture", result.picture_path}, {"pictures", pictures},
                    {"sees", chat->held.model->sees_images()}};
    }
    if (method == "turn.search") {
        const tools::SearchSettings settings = search_settings(chat->config);
        const std::string query = params.value("query", std::string());
        std::string error;
        const std::vector<tools::SearchResult> results = tools::search(query, settings, error);
        return json{{"count", results.size()}, {"error", error}, {"provider", settings.provider},
                    {"text", tools::format_for_model(query, results)}};
    }

    // --- what the transcript says ----------------------------------------------
    const auto show = [&] {
        for (const json& one : params.value("actions", json::array())) {
            state_.add_action(turn, TurnAction{one.value("summary", std::string()), one.value("body", std::string()),
                                               one.value("language", std::string()),
                                               one.value("picture", std::string())});
        }
        if (params.contains("reply") && params["reply"].is_string()) {
            state_.set_reply(turn, params["reply"].get<std::string>());
        }
    };
    if (method == "turn.show") {
        show();
        if (wake_) {
            wake_();
        }
        return json::object();
    }
    if (method == "turn.end") {
        if (params.value("remember", false)) {
            // The exchange, for the turns after this one: the question with
            // what was attached, so the next question can be about it too --
            // it is the first thing trimmed when room runs short -- and the
            // reply as the expert left it.
            const Snapshot now = state_.snapshot();
            if (turn < now.turns.size()) {
                const std::lock_guard<std::mutex> lock(mutex_);
                history_.push_back(chat->asked);
                history_.push_back({"assistant", now.turns[turn].reply});
            }
        }
        show();
        const std::string state = params.value("state", std::string("finished"));
        if (state == "failed") {
            state_.fail_turn(turn, params.value("reason", std::string()));
        } else if (state == "canceled") {
            state_.cancel_turn(turn);
        } else {
            state_.finish_turn(turn, chat->stats, chat->load_ms);
        }
        // The work has stopped, so the line goes and the seat goes dark.
        // Whether the weights are still in memory is a separate question, and
        // the status bar is where it is answered.
        if (params.value("unlink", true)) {
            state_.set_linked(std::nullopt);
        }
        state_.set_mood(mood_named(params.value("mood", std::string("idle"))),
                        params.value("status", std::string()));
        chat->ended = true;
        if (wake_) {
            wake_();
        }
        return json::object();
    }

    throw std::runtime_error("the core has no method " + method);
}

}  // namespace crucible
