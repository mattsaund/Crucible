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

#include <algorithm>
#include <chrono>
#include <map>
#include <stdexcept>

#include "crucible/config/paths.hpp"
#include "crucible/engine/overflow.hpp"
#include "crucible/lab/python.hpp"
#include "crucible/llm/loaded_model.hpp"
#include "crucible/llm/response_filter.hpp"
#include "crucible/tools/native_tools.hpp"
#include "crucible/tools/web_search.hpp"
#include "crucible/tools/workshop.hpp"
#include "crucible/util/text.hpp"

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

tools::ToolKind tool_kind_from_name(const std::string& name) {
    for (const tools::ToolKind kind :
         {tools::ToolKind::List, tools::ToolKind::Read, tools::ToolKind::Write, tools::ToolKind::Edit,
          tools::ToolKind::Find, tools::ToolKind::Render, tools::ToolKind::Mcp, tools::ToolKind::Run,
          tools::ToolKind::Search, tools::ToolKind::Ask, tools::ToolKind::Note, tools::ToolKind::Done,
          tools::ToolKind::Handoff, tools::ToolKind::Fetch, tools::ToolKind::Git, tools::ToolKind::Gh,
          tools::ToolKind::Python, tools::ToolKind::Start, tools::ToolKind::Stop, tools::ToolKind::Logs,
          tools::ToolKind::Screenshot, tools::ToolKind::Click, tools::ToolKind::Move,
          tools::ToolKind::Type, tools::ToolKind::Key, tools::ToolKind::Scroll}) {
        if (tools::tool_kind_name(kind) == name) {
            return kind;
        }
    }
    return tools::ToolKind::None;
}

Mood mood_named(const std::string& mood) {
    return mood == "thinking" ? Mood::Thinking
         : mood == "loading"  ? Mood::Loading
         : mood == "routing"  ? Mood::Routing
         : mood == "error"    ? Mood::Error
                              : Mood::Idle;
}

/// The most context a turn with the tools is given, when the cards hold it.
/// Its rounds are long: the request, an account of what was done, the tools,
/// the last few exchanges with the files they read, and the thinking before
/// the next action, all in one context. A ceiling, not a demand:
/// ModelHost::load gives what the cards hold of it and never less than the
/// seat's own context. gpt-oss-20b at 32K is 13.1 GB on a card by its header,
/// against 12.0 at 8K.
constexpr int kWorkContext = 32768;

/// How often, in pieces of output, a round looks at whether its reply has
/// started going round in a loop. Often enough to stop one early, and rarely
/// enough to cost nothing.
constexpr std::size_t kLoopLook = 16;

/// What the checkpoint may write on a model that cannot leave its thinking
/// out, in tokens: enough to think and still say the next step.
constexpr int kCheckpointThinking = 768;

}  // namespace

tools::ToolCall Engine::call_from(const json& params) {
    tools::ToolCall call;
    call.kind     = tool_kind_from_name(params.value("kind", ""));
    call.argument = params.value("argument", "");
    call.content  = params.value("content", "");
    call.shell    = params.value("shell", "");
    call.unrun    = params.value("unrun", "");
    return call;
}

json Engine::parse_reply(const json& params) {
    const std::string answer    = params.value("answer", "");
    const std::string reasoning = params.value("reasoning", "");
    std::optional<tools::ToolCall> call = tools::parse_tool_call(answer, reasoning);
    // A call the model spelled out in its own format, and what it said first.
    std::optional<std::string> prose;
    if (!call) {
        if (std::optional<tools::CallInText> written = tools::call_in_text(answer)) {
            call  = std::move(written->call);
            prose = std::move(written->prose);
        }
    }
    // And what it asked to look up, if anything. A search is asked for in a
    // way of its own -- a SEARCH line, or a tool-trained model's own call on
    // its reasoning channel -- and has bookkeeping of its own. See
    // web_search.hpp.
    const std::string search = tools::search_request(answer, reasoning);
    if (!call) {
        // A near miss, named so the loop can say what was wrong. A WRITE or an
        // EDIT after a sentence on its line is one as well, "inline": it was
        // the place on the line that was wrong, not the colon.
        tools::ToolKind attempted = tools::attempted_tool_call(answer, reasoning);
        bool            inside    = false;
        if (attempted == tools::ToolKind::None) {
            attempted = tools::call_inside_a_line(answer, reasoning);
            inside    = attempted != tools::ToolKind::None;
        }
        // A call in the model's own format that could not be read. The bare
        // tag must never be left standing as what the expert answered.
        bool in_its_format = false;
        for (const std::string_view mark : {std::string_view("<tool_call>"), std::string_view("<|tool_call_start|>"),
                                            std::string_view("<function="), std::string_view("to=functions.")}) {
            in_its_format = in_its_format || answer.find(mark) != std::string::npos;
        }
        if (attempted == tools::ToolKind::None && in_its_format) {
            return json{{"call", nullptr}, {"attempted", "call"}, {"inline", false}, {"search", search}};
        }
        return json{{"call", nullptr},
                    {"attempted", std::string(tools::tool_kind_name(attempted))},
                    {"inline", inside},
                    {"search", search}};
    }
    // All of it, because the orchestrator hands the call straight back to
    // turn.tool: without the shell, "RUN powershell:" ran under cmd, and
    // without `unrun` a second call in the answer went unmentioned.
    json parsed{{"call", {{"kind", std::string(tools::tool_kind_name(call->kind))},
                          {"argument", call->argument},
                          {"content", call->content},
                          {"shell", call->shell},
                          {"unrun", call->unrun}}},
                {"attempted", "none"},
                {"search", search}};
    if (prose) {
        parsed["prose"] = *prose;
    }
    // A search written that way goes the way a search goes.
    if (call->kind == tools::ToolKind::Search) {
        parsed["call"]   = nullptr;
        parsed["search"] = call->argument;
    }
    return parsed;
}

void Engine::set_project(std::filesystem::path root) {
    project_root_ = std::move(root);
}

tools::WorkshopSettings Engine::workshop_for(const std::filesystem::path& root) const {
    tools::WorkshopSettings workshop;
    // Having a root is the permission. It is only ever set to a folder the user
    // answered the trust question for, and that question is asked in the terms
    // this is actually about: read, write, run, inside this directory.
    workshop.enabled             = !root.empty();
    workshop.root                = root;
    workshop.allow_run           = !root.empty();
    {
        // Under the lock: Settings can be applied while a turn reads these.
        const std::lock_guard<std::mutex> lock(config_mutex_);
        workshop.run_timeout_seconds = config_.tools.workshop_timeout;
        // The rest of what an expert can reach, each behind its own switch.
        workshop.web              = config_.tools.web_search;
        workshop.computer_control = config_.tools.computer_control;
    }
    workshop.python           = lab::python::installed() ? lab::python::interpreter()
                                                         : std::filesystem::path();
    workshop.scratch          = paths::scratch_dir();
    workshop.processes        = const_cast<tools::Processes*>(&processes_);
    workshop.mcp              = const_cast<tools::mcp::Hub*>(&mcp_);
    return workshop;
}

/// What the core keeps of the chat turn being answered, between the calls the
/// orchestrator makes of it.
struct Engine::ChatTurn {
    std::size_t                     index = 0;   ///< the turn, in the transcript
    std::string                     prompt;
    std::vector<attach::Attachment> attachments;
    std::optional<ExpertId>         pinned;      ///< sent to one seat

    /// The settings as they are when the turn starts, kept for the whole of
    /// it: Settings can be applied while it runs, and a turn read halfway
    /// through a change is a turn answered by half of two configurations.
    Config config;

    // Who answers, once the turn is routed and the expert seated.
    ExpertId                expert;
    bool                    routed = false;
    std::string             name;      ///< the expert's name, for the status line
    ModelParams             params;
    HeldModel               held;      ///< the model, held until the turn is over
    long                    load_ms = 0;
    tools::WorkshopSettings workshop;

    /// The seated model is offered the tools as functions, through its own
    /// chat template, and calls them the way it was trained to. See
    /// tools/native_tools.hpp. One with no tools in its template, and a
    /// provider's, reads the `VERB:` lines instead.
    bool                   native = false;
    nlohmann::ordered_json native_tools;

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
    bool thought_shown = false;   ///< this turn has shown some reasoning already
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
    asked["actions"]       = chat->config.tools.actions;
    asked["root"]          = project_root_.string();
    asked["search_tools"]  = tools::tool_instructions();
    asked["overflow"]      = chat->config.tools.overflow;

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
    // An orchestrator that went away mid-turn has nobody left to read the
    // reply: what it asked for stops as Stop would. See Link::ended.
    const CancelCallback cancel = [this, asker = orchestra::Link::asker()] {
        return cancel_.load(std::memory_order_relaxed) || !running_.load(std::memory_order_relaxed)
            || orchestra_.ended(asker);
    };

    // --- who answers ---------------------------------------------------------
    if (method == "turn.route") {
        const json decided = params.value("decision", json::object());
        RouteDecision decision;
        decision.expert     = decided.value("expert", "");
        decision.confidence = decided.value("confidence", 0.0F);
        decision.source     = route_source_from_name(decided.value("source", "fallback"));
        decision.detail     = decided.value("detail", "");
        // Its work for this prompt is done -- or there was none, for a prompt
        // sent to one expert -- and it goes if the expert is about to want
        // the room it holds. Not when the two fit side by side, or the expert
        // is somewhere else: freeing it would only mean loading it again for
        // the next prompt.
        make_room_for(chat->config.expert(decision.expert));
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
        // The tools a turn gets, in a folder the person has trusted. An empty
        // root is a workshop with nothing in it to offer. A turn that has them
        // may work for a while, and is given the room for it.
        chat->workshop = workshop_for(project_root_);
        if (chat->workshop.enabled) {
            chat->params.n_ctx_most = std::max(chat->params.n_ctx, kWorkContext);
        }
        // A turn changing hands lets go of the model it had first. Held, that
        // model is in use, and the next expert's load waits for the room it
        // takes up until the turn ends, which it never would: flint, on all
        // three cards, kept GPT OSS from loading for half an hour.
        chat->held = HeldModel{};
        std::string error;
        // Held for the whole turn: the lease is what keeps anybody else from
        // freeing this model while the turn is answering with it.
        chat->held = hold_model(chat->expert, chat->params, chat->name, cancel, chat->load_ms, error);
        if (chat->held.model == nullptr) {
            return json{{"ok", false}, {"error", error}};
        }
        const ChatModel& model       = *chat->held.model;
        chat->workshop.sees_pictures = model.sees_images();
        auto* const local            = dynamic_cast<LoadedModel*>(chat->held.model);
        chat->native                 = chat->workshop.enabled && local != nullptr && local->speaks_tools();
        chat->native_tools           = chat->native
                                           ? tools::native_tools(chat->workshop, chat->config.tools.web_search)
                                           : nlohmann::ordered_json();
        const std::string lines      = tools::workshop_instructions(chat->workshop);
        return json{{"ok", true}, {"remote", chat->params.remote()},
                    {"sees_images", model.sees_images()}, {"takes_effort", model.takes_effort()},
                    {"workshop", chat->workshop.enabled},
                    {"native", chat->native},
                    {"tools", chat->native ? tools::native_instructions(chat->workshop) : lines},
                    {"text_tools", lines}};
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
        // Instructions for this round alone, when the orchestrator sends
        // them: the round that answers from what the turn found is offered no
        // tools. The conversation is fitted with the longer ones, so these fit.
        if (const auto system = params.find("system");
            system != params.end() && system->is_string() && !messages.empty() && messages.front().role == "system") {
            messages.front().content = system->get<std::string>();
        }
        // What the turn did before the messages it is still shown, beside the
        // request rather than in the instructions, which stay as they were so
        // that the model does not read everything after them again.
        if (const std::string note = params.value("note", std::string()); !note.empty() && messages.size() > 1) {
            messages.back().content += "\n\n" + note;
        }
        // Asked through the model's own template, the turn's exchanges go as
        // they are, the calls it made and what came back from them included.
        // A picture is a part of a message of its own after the result.
        const bool             native = chat->native && params.value("native", false);
        json                   wire   = json::array();
        std::vector<ChatImage> wire_images;
        const auto             wired = [&wire_images](const ChatMessage& message) {
            if (message.images.empty()) {
                return json{{"role", message.role}, {"content", message.content}};
            }
            json parts = json::array();
            for (const ChatImage& image : message.images) {
                parts.push_back(json{{"type", "media_marker"}});
                wire_images.push_back(image);
            }
            parts.push_back(json{{"type", "text"}, {"text", message.content}});
            return json{{"role", message.role}, {"content", std::move(parts)}};
        };
        if (native) {
            for (const ChatMessage& message : messages) {
                wire.push_back(wired(message));
            }
        }
        for (const json& one : params.value("messages", json::array())) {
            const auto held = chat->pictures.find(one.value("pictures", std::string()));
            if (native) {
                json entry = one;
                entry.erase("pictures");
                wire.push_back(std::move(entry));
                if (held != chat->pictures.end()) {
                    ChatMessage shown{"user", "That is the picture."};
                    shown.images = held->second;
                    wire.push_back(wired(shown));
                }
                continue;
            }
            ChatMessage message{one.value("role", "user"), one.value("content", "")};
            if (held != chat->pictures.end()) {
                message.images = held->second;
            }
            messages.push_back(std::move(message));
        }
        // A paragraph between what the rounds before this one left on screen
        // and what this one says, rather than one run into the other.
        const bool        paragraph    = params.value("paragraph", false);
        // A round the person does not see: the loop asking the expert what the
        // next step is, so the delegator can choose who takes it.
        const bool        quiet        = params.value("quiet", false);
        // A round asked for one line, which a model that reasons would spend
        // on reasoning about it: the checkpoint.
        const bool        quick        = params.value("quick", false);
        // The round after one that thought for too long, which is to act.
        const bool        rush         = params.value("rush", false);
        // How much reasoning a round may write before it is stopped, or zero.
        const std::size_t think_most   = params.value("think_most", std::size_t{0});
        bool              overthought  = false;
        // Set once the reply has started going round, a few words or a dot
        // again and again. Stopped there rather than run to the end of the
        // context: gpt-oss once wrote 7177 dots.
        bool              looped       = false;
        bool              first_answer = true;
        bool              first_thought = true;
        std::string       answer;
        std::string       reasoning;
        ResponseFilter    filter;
        const std::string name = chat->name;

        // Thinking, from here until it says or does something. Not the last
        // step's words, which read as a command still running long after it
        // had finished, or as a load still going while it read the prompt.
        state_.set_mood(Mood::Thinking, name + " is thinking");
        if (wake_) {
            wake_();
        }

        // The two things a piece of output can be, wherever it came from.
        const auto take = [&](std::string_view thought, std::string_view said) {
            if (chat->first_token) {
                chat->first_token    = false;
                chat->first_token_at = Clock::now();
                state_.set_mood(Mood::Thinking, name + " is thinking");
            }
            if (!thought.empty()) {
                reasoning += thought;
                if (!quiet) {
                    // Each round's thinking is a paragraph of its own: run
                    // together they read "created the file.The file has".
                    if (first_thought && chat->thought_shown) {
                        state_.append_reasoning(turn, "\n\n");
                    }
                    first_thought       = false;
                    chat->thought_shown = true;
                    state_.append_reasoning(turn, thought);
                }
                if (think_most > 0 && answer.empty() && reasoning.size() > think_most) {
                    overthought = true;
                }
            }
            if (!said.empty()) {
                if (first_answer) {
                    first_answer = false;
                    if (!quiet) {
                        state_.set_mood(Mood::Talking, name + " is answering");
                        if (paragraph) {
                            state_.append_reply(turn, "\n\n");
                        }
                    }
                }
                answer += said;
                if (!quiet) {
                    state_.append_reply(turn, said);
                }
            }
            if (chat->streamed_chunks % kLoopLook == 0 && !looped) {
                looped = detail::ends_in_loop(answer) || detail::ends_in_loop(reasoning);
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
        sink.cancel = [&cancel, &looped, &overthought] { return looped || overthought || cancel(); };
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

        // Thinking left out where the template can leave it out. gpt-oss's
        // cannot, and writes its effort into the system message, so a
        // different one reads the whole prompt again and again for the round
        // after: the checkpoint keeps it and has the room to think instead.
        // Only a round that has already thought too long is worth that.
        std::string effort = chat->config.reasoning_effort;
        int         most   = params.value("most", 0);
        if (native && (quick || rush)) {
            if (!model.takes_effort()) {
                effort = "none";
            } else if (rush) {
                effort = "low";
            } else if (most > 0) {
                most = std::max(most, kCheckpointThinking);
            }
        }

        // A round that is asked for one line may write only so much of it.
        ModelParams asked_params = chat->params;
        if (most > 0) {
            asked_params.max_tokens = asked_params.max_tokens > 0 ? std::min(asked_params.max_tokens, most) : most;
        }
        ChatResult outcome;
        json       calls     = json::array();
        bool       no_native = false;
        int        too_long  = 0;
        if (native) {
            OpenChat open;
            open.messages            = std::move(wire);
            open.images              = std::move(wire_images);
            open.tools_text          = params.value("tools", true) ? chat->native_tools.dump() : std::string();
            open.tool_choice         = params.value("tools", true) ? "auto" : "none";
            open.parallel_tool_calls = false;
            open.must_call           = quick && params.value("tools", true) ? std::string(tools::kHandOffTool)
                                                                            : std::string();
            open.effort              = effort;
            open.params              = asked_params;
            OpenChatSink open_sink;
            open_sink.cancel   = sink.cancel;
            open_sink.on_delta = [&](const OpenChatDelta& delta) {
                if (!delta.reasoning.empty() || !delta.content.empty()) {
                    take(delta.reasoning, delta.content);
                }
            };
            const OpenChatResult result = dynamic_cast<LoadedModel&>(model).open_chat(open, open_sink);
            outcome.stats = result.stats;
            too_long      = result.too_long;
            // A template that will not take this conversation, or these tools,
            // leaves the lines every model can be told to write.
            no_native = result.refused || result.untemplated;
            if (!no_native && too_long == 0) {
                outcome.error = result.error;
            }
            for (const ToolCall& call : result.calls) {
                calls.push_back(json{{"id", call.id}, {"name", call.name}, {"arguments", call.arguments}});
            }
        } else {
            outcome = model.chat(ChatRequest{messages, asked_params, chat->config.reasoning_effort}, sink);
        }
        const GenerationStats& pass = outcome.stats;

        // Whatever was still held back, waiting to see if it was a marker.
        if (const ResponseFilter::Piece last = filter.flush(); !last.answer.empty() || !last.reasoning.empty()) {
            if (!quiet) {
                state_.append_reasoning(turn, last.reasoning);
                state_.append_reply(turn, last.answer);
            }
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
            stats.canceled       = pass.canceled && !looped && !overthought;
            stats.hit_limit      = pass.hit_limit;
        }
        const Snapshot now = state_.snapshot();
        return json{{"answer", answer},
                    {"reasoning", reasoning},
                    {"error", outcome.error},
                    {"discard", outcome.discard_partial},
                    {"notes", outcome.notes},
                    {"canceled", pass.canceled && !looped && !overthought},
                    {"looped", looped},
                    {"overthought", overthought},
                    {"hit_limit", pass.hit_limit},
                    {"output_tokens", pass.output_tokens},
                    {"prompt_tokens", pass.prompt_tokens},
                    {"prompt_reused", pass.prompt_reused},
                    {"prompt_ms", pass.prompt_ms},
                    {"output_ms", pass.output_ms},
                    {"calls", std::move(calls)},
                    {"no_native", no_native},
                    {"too_long", too_long},
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
