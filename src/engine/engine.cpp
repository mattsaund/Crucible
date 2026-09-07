// SPDX-License-Identifier: MIT
//
// The delegation loop.
//
// Everything llama.cpp touches happens on this thread. The UI hands over a
// prompt and learns how it went through AppState plus a wake callback -- it
// never calls into the engine's internals, and the engine never calls into
// FTXUI.
//
// Config changes and expert releases ride the same queue as prompts, so they
// are applied in order and can never land in the middle of a generation.
#include "crucible/engine/engine.hpp"

#include <algorithm>
#include <chrono>
#include <exception>

#include "crucible/config/gpu_policy.hpp"
#include "crucible/config/paths.hpp"
#include "crucible/engine/route_policy.hpp"
#include "crucible/llm/response_filter.hpp"
#include "crucible/tools/web_search.hpp"
#include "crucible/tools/workshop.hpp"
#include "crucible/runtime/registry.hpp"

namespace crucible {
namespace {

/// How many times one chat turn may act on the project before it has to answer.
///
/// Higher than the search ceiling and counting a different thing. Reading a
/// file, rewriting it and running the tests is three rounds of honest work; the
/// same number of searches is a model going round in circles. Bounded all the
/// same, because a turn that never writes an answer is a turn the person who
/// asked gets nothing from.
constexpr int kToolRounds = 12;

/// Is this verb an action on the project, rather than an answer to the cook
/// loop or a web search?
///
/// SEARCH has its own path below. ASK, DONE and HANDOFF are how a cook ends a
/// piece of work and are not offered to a chat turn at all -- if one arrives
/// anyway it falls through to being shown, which is the right outcome: a model
/// that writes "DONE: fixed it" has said something to the reader.
bool is_project_verb(tools::ToolKind kind) {
    switch (kind) {
        case tools::ToolKind::List:
        case tools::ToolKind::Read:
        case tools::ToolKind::Write:
        case tools::ToolKind::Run:
        case tools::ToolKind::Note:
            return true;
        case tools::ToolKind::None:
        case tools::ToolKind::Search:
        case tools::ToolKind::Ask:
        case tools::ToolKind::Done:
        case tools::ToolKind::Handoff:
            break;
    }
    return false;
}


using Clock = std::chrono::steady_clock;

long ms_since(Clock::time_point start) {
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

/// How many past turns to replay to an expert.
///
/// Bounded deliberately: a swapped-in expert re-ingests the whole history from
/// cold, so an unbounded transcript would make every turn slower than the last.
constexpr std::size_t kHistoryTurns = 12;

}  // namespace

Engine::Engine(Config config, AppState& state, std::function<void()> wake)
    : config_(std::move(config)), state_(state), wake_(std::move(wake)) {}

Engine::~Engine() {
    stop();
}

void Engine::start() {
    if (running_.exchange(true)) {
        return;
    }
    worker_ = std::thread(&Engine::run, this);
}

void Engine::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    cancel_.store(true, std::memory_order_relaxed);
    queued_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Engine::submit(std::string prompt, std::optional<ExpertId> pinned) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        Request request;
        request.kind   = RequestKind::Prompt;
        request.prompt = std::move(prompt);
        request.pinned = std::move(pinned);
        pending_.push_back(std::move(request));
    }
    queued_.notify_one();
}

void Engine::write_examples(ExpertId id) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        Request request;
        request.kind   = RequestKind::WriteExamples;
        request.expert = std::move(id);
        pending_.push_back(std::move(request));
    }
    queued_.notify_one();
}

std::vector<std::pair<ExpertId, std::vector<std::string>>> Engine::take_written_examples() {
    const std::lock_guard<std::mutex> lock(written_mutex_);
    return std::exchange(written_examples_, {});
}

void Engine::apply_config(Config config) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        Request request;
        request.kind   = RequestKind::ApplyConfig;
        request.config = std::move(config);
        pending_.push_back(std::move(request));
    }
    queued_.notify_one();
}

Config Engine::config() const {
    const std::lock_guard<std::mutex> lock(config_mutex_);
    return config_;
}

void Engine::cancel() {
    cancel_.store(true, std::memory_order_relaxed);
}

void Engine::release_expert() {
    // Queued rather than done inline: host_ belongs to the worker thread, and
    // reaching into it from the UI thread mid-generation would be a data race.
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        Request request;
        request.kind = RequestKind::ReleaseExpert;
        pending_.push_back(std::move(request));
    }
    queued_.notify_one();
}

void Engine::reload_models() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        Request request;
        request.kind = RequestKind::ReloadModels;
        pending_.push_back(std::move(request));
    }
    queued_.notify_one();
}

void Engine::reset_history() {
    const std::lock_guard<std::mutex> lock(mutex_);
    history_.clear();
}

void Engine::restore_history(std::vector<ChatMessage> history) {
    const std::lock_guard<std::mutex> lock(mutex_);
    history_ = std::move(history);
}

void Engine::run() {
    // Constructing the host loads the runtimes, so nothing before this point
    // knows what hardware exists -- which is why the GPU policy is applied
    // here rather than when the config was parsed.
    host_ = std::make_unique<ModelHost>(paths::log_file());

    const std::vector<std::string> devices = ModelHost::devices();
    for (const std::string& device : devices) {
        state_.add_notice("device: " + device);
    }
    // Said at startup rather than at the first prompt: with no runtime there
    // is no hardware to run a model on, and finding that out only when you
    // have typed a question is the worse way to learn it.
    if (devices.empty()) {
        state_.add_notice(RuntimeRegistry::any_installed()
                              ? "a runtime is installed but found no hardware it can drive "
                                "-- type /runtimes"
                              : "no runtime installed -- type /runtimes and install one "
                                "before assigning models");
    }

    {
        const std::lock_guard<std::mutex> lock(config_mutex_);
        if (const std::string split = apply_gpu_policy(config_); !split.empty()) {
            state_.add_notice("GPU split (" + config_.gpu.mode + "): " + split);
        }
        // Every load re-plans its own split from live memory, and the host is
        // where that happens. See refresh_gpu_split.
        host_->set_gpu_config(config_.gpu);
    }

    load_router_if_resident();
    state_.configure_seats(config_);
    state_.set_mood(Mood::Idle);
    if (wake_) {
        wake_();
    }

    while (running_.load(std::memory_order_relaxed)) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            queued_.wait(lock, [this] {
                return !pending_.empty() || !running_.load(std::memory_order_relaxed);
            });
            if (!running_.load(std::memory_order_relaxed)) {
                break;
            }
            request = std::move(pending_.front());
            pending_.pop_front();
        }

        if (request.kind == RequestKind::ReloadModels) {
            // Order matters: free everything first, then reload. Loading the
            // router while the old expert is still resident would need both in
            // memory at once, which on a machine that was just given a GPU is
            // the moment least likely to have room for it.
            host_->release_expert();
            state_.set_resident(std::nullopt);
            router_.reset();

            // The device list is exactly what just changed, and the split was
            // worked out from the old one.
            {
                const std::lock_guard<std::mutex> lock(config_mutex_);
                if (const std::string split = apply_gpu_policy(config_); !split.empty()) {
                    state_.add_notice("GPU split (" + config_.gpu.mode + "): " + split);
                }
                host_->set_gpu_config(config_.gpu);
            }

            load_router_if_resident();
            state_.set_mood(Mood::Idle, "runtime changed");
            if (wake_) {
                wake_();
            }
            continue;
        }

        if (request.kind == RequestKind::ReleaseExpert) {
            host_->release_expert();
            state_.set_resident(std::nullopt);
            state_.set_mood(Mood::Idle, "expert released");
            if (wake_) {
                wake_();
            }
            continue;
        }

        if (request.kind == RequestKind::ApplyConfig) {
            do_apply_config(std::move(request.config));
            if (wake_) {
                wake_();
            }
            continue;
        }

        if (request.kind == RequestKind::Cook) {
            busy_.store(true, std::memory_order_relaxed);
            state_.set_busy(true);
            // Contained like every other request. A cook is an hour of running
            // whatever a model asks for, so an exception escaping here would
            // take the process down and leave the terminal in raw mode.
            try {
                do_cook(request.prompt, request.budget_seconds, request.root);
            } catch (const std::exception& e) {
                state_.set_mood(Mood::Error, e.what());
                state_.add_notice(std::string("cook failed: ") + e.what());
                cooking_.store(false, std::memory_order_relaxed);
            } catch (...) {
                state_.set_mood(Mood::Error, "cook failed");
                cooking_.store(false, std::memory_order_relaxed);
            }
            busy_.store(false, std::memory_order_relaxed);
            state_.set_busy(false);
            if (wake_) {
                wake_();
            }
            continue;
        }

        if (request.kind == RequestKind::WriteExamples) {
            // Wrapped for the same reason a prompt is: llama.cpp reports a
            // corrupt GGUF by throwing, and a new expert failing to get its
            // examples must not take the session down with it.
            try {
                do_write_examples(request.expert);
            } catch (const std::exception& e) {
                state_.add_notice(std::string("could not write examples: ") + e.what());
            } catch (...) {
                state_.add_notice("could not write examples");
            }
            if (wake_) {
                wake_();
            }
            continue;
        }

        if (request.prompt.empty()) {
            continue;
        }

        busy_.store(true, std::memory_order_relaxed);
        state_.set_busy(true);
        cancel_.store(false, std::memory_order_relaxed);

        // llama.cpp reports some failures (a malformed grammar, a corrupt
        // GGUF) by throwing. Letting that escape the worker would terminate
        // the process and leave the user's terminal in raw mode, so every
        // request is contained: the turn fails, the session survives.
        try {
            handle(request);
        } catch (const std::exception& e) {
            state_.set_mood(Mood::Error, e.what());
            state_.add_notice(std::string("engine error: ") + e.what());
        } catch (...) {
            state_.set_mood(Mood::Error, "unknown engine error");
            state_.add_notice("engine error: unknown exception");
        }

        busy_.store(false, std::memory_order_relaxed);
        state_.set_busy(false);
        if (wake_) {
            wake_();
        }
    }

    // Free the models before the backend goes away.
    router_.reset();
    host_.reset();
}

void Engine::load_router() {
    if (config_.router.model.empty()) {
        router_ = std::make_unique<KeywordRouter>(
            std::make_shared<const Roster>(config_.roster));
        state_.add_notice("no delegator model assigned");
        state_.set_delegator_ready(true);  // keywords need nothing loaded
        return;
    }

    state_.set_mood(Mood::Loading, "loading router");
    if (wake_) {
        wake_();
    }

    std::string error;
    LoadedModel* model = host_->acquire_router(
        config_.router,
        [this](float progress) {
            state_.set_mood(Mood::Loading,
                            "loading router " + std::to_string(static_cast<int>(progress * 100))
                                + "%");
            if (wake_) {
                wake_();
            }
        },
        error);

    if (model == nullptr) {
        router_ = std::make_unique<KeywordRouter>(
            std::make_shared<const Roster>(config_.roster));
        state_.add_notice("router: " + error + " -- falling back to keyword routing");
        state_.set_delegator_ready(true);
        return;
    }

    auto routed = std::make_unique<ModelRouter>(
        *model, config_.router, std::make_shared<const Roster>(config_.roster));
    if (router_bias_for_ == config_.router.path) {
        routed->set_bias(router_bias_);
    }
    router_ = std::move(routed);
    state_.set_delegator_ready(true);
}

void Engine::ensure_router() {
    if (router_ && host_->router() != nullptr) {
        return;  // already there
    }
    if (config_.router.model.empty()) {
        if (!router_) {
            router_ = std::make_unique<KeywordRouter>(
            std::make_shared<const Roster>(config_.roster));
        }
        return;
    }
    load_router();
}

/// Load the delegator now, unless it is set to load on demand -- in which case
/// putting it in memory before the first prompt is exactly what this mode
/// exists to avoid, and Engine::resolve will fetch it when a decision is
/// actually needed.
void Engine::load_router_if_resident() {
    if (config_.routing.keep_delegator_loaded || config_.router.model.empty()) {
        load_router();
        return;
    }
    state_.add_notice("delegator loads on demand -- it is freed after each decision");
}

void Engine::release_router() {
    // The wrapper holds a reference to the loaded model, so it goes first --
    // and its calibration is kept, because the next load is the same file.
    if (const auto* routed = dynamic_cast<const ModelRouter*>(router_.get())) {
        router_bias_     = routed->bias();
        router_bias_for_ = config_.router.path;
    }
    router_.reset();
    host_->release_router();
    state_.set_delegator_ready(false);
}

void Engine::do_apply_config(Config config) {
    config.resolve_models();
    apply_gpu_policy(config);

    std::string previous_router;
    std::string previous_expert;
    std::optional<ExpertId> resident = host_->loaded_expert();
    {
        const std::lock_guard<std::mutex> lock(config_mutex_);
        previous_router = config_.router.path;
        if (resident) {
            previous_expert = config_.expert(*resident).path;
        }
        config_ = std::move(config);
        host_->set_gpu_config(config_.gpu);
    }

    const Config current = this->config();

    // Drop the resident expert if the file behind its seat changed, so the next
    // prompt loads what the user just chose rather than the old weights.
    if (resident) {
        // An expert whose seat has been ejected outright reads as an empty
        // path here, which takes the same branch as one whose file changed:
        // drop it. That is what makes /ejectexpert free the weights of the
        // expert it just removed rather than leaving them resident and
        // unreachable.
        const std::string now = current.expert(*resident).path;
        if (now != previous_expert || now.empty()) {
            host_->release_expert();
            state_.set_resident(std::nullopt);
        }
    }

    // The router is resident for the whole session, so a change to it has to be
    // acted on here or it would never take effect.
    if (current.router.path != previous_router) {
        release_router();
        router_bias_.clear();
        router_bias_for_.clear();
        load_router_if_resident();
    }

    state_.configure_seats(current);
    state_.set_resident(host_->loaded_expert());
    state_.set_mood(Mood::Idle, "settings applied");
}

void Engine::do_write_examples(const ExpertId& id) {
    const std::optional<std::size_t> seat = config_.roster.find(id);
    if (!seat) {
        return;  // ejected again before this ran, which is a perfectly good answer
    }
    const Expert expert = config_.roster.at(*seat);
    if (!expert.examples.empty()) {
        return;  // already has them; this is not a rewrite
    }

    ensure_router();
    LoadedModel* model = host_->router();
    if (model == nullptr) {
        return;  // no delegator: the seat still routes on its blurb and keywords
    }

    state_.set_mood(Mood::Thinking, "writing examples for " + expert.name);
    if (wake_) {
        wake_();
    }

    // Sampled rather than scored, and warmer than routing: two questions that
    // are near-copies of each other teach the delegator nothing, and greedy
    // decoding on a short prompt produces exactly that.
    ModelParams params = config_.router;
    params.temperature = 0.6F;
    params.max_tokens  = 128;

    const std::vector<ChatMessage> messages{
        {"user", example_request_prompt(expert.name, expert.blurb)}};

    std::string reply;
    const CancelCallback cancel = [this] { return cancel_.load(std::memory_order_relaxed); };
    model->generate(model->format_chat(messages, true), params,
                    [&reply](std::string_view chunk) { reply += chunk; }, cancel);

    std::vector<std::string> examples = parse_examples(reply);
    state_.set_mood(Mood::Idle);
    if (examples.empty()) {
        // A small delegator can fail to follow the format, and that is not
        // worth a warning: the seat works, it is simply routed to by blurb and
        // keyword alone.
        return;
    }

    {
        const std::lock_guard<std::mutex> lock(written_mutex_);
        written_examples_.emplace_back(id, examples);
    }

    // Folded into the engine's own copy as well, so the delegator built for the
    // next prompt already has them -- the UI's copy is updated separately when
    // it drains the outbox, and waiting for that round trip would mean the
    // first prompt after adding an expert routed without the examples that were
    // just written for it.
    Expert updated = expert;
    updated.examples = std::move(examples);
    {
        const std::lock_guard<std::mutex> lock(config_mutex_);
        config_.roster.update(id, updated);
    }
    // The router holds a snapshot of the roster taken when it was built, so it
    // has to be rebuilt for the new examples to reach it.
    router_.reset();
    load_router_if_resident();
}

RouteDecision Engine::resolve(const Request& request) {
    const CancelCallback cancel = [this] { return cancel_.load(std::memory_order_relaxed); };

    // A pinned route needs no delegator at all, which is worth saying twice:
    // with "keep delegator loaded" off, a slash command costs nothing to route.
    RouteDecision decision;
    if (request.pinned) {
        decision.expert     = *request.pinned;
        decision.confidence = 1.0F;
        decision.source     = RouteSource::Forced;
        decision.detail     = "pinned by slash command";
        return apply_route_policy(decision, config_);
    }

    ensure_router();
    if (router_) {
        decision = router_->route(request.prompt, cancel);
    }
    if (!config_.routing.keep_delegator_loaded) {
        // Its work for this prompt is done, and the expert is about to want
        // every byte it was holding.
        release_router();
    }

    // Then decide what to do about it.
    return apply_route_policy(decision, config_);
}

void Engine::handle(const Request& request) {
    const std::size_t turn = state_.begin_turn(request.prompt);

    const CancelCallback cancel = [this] { return cancel_.load(std::memory_order_relaxed); };

    // --- route -------------------------------------------------------------
    state_.set_mood(Mood::Routing, "Crucible is reading the prompt");
    if (wake_) {
        wake_();
    }

    const RouteDecision decision = resolve(request);
    state_.set_route(turn, decision);
    // From here the expert panel draws a line from Crucible to this seat.
    state_.set_linked(decision.expert);
    if (wake_) {
        wake_();
    }

    if (cancel_.load(std::memory_order_relaxed)) {
        state_.fail_turn(turn, "cancelled before routing finished");
        state_.set_linked(std::nullopt);
        state_.set_mood(Mood::Idle);
        return;
    }

    if (!config_.has_expert(decision.expert)) {
        state_.fail_turn(turn,
            "No expert model is configured yet. Edit " + paths::config_file().string()
            + " and point at least one expert at a GGUF file.");
        state_.set_linked(std::nullopt);
        state_.set_mood(Mood::Error, "no experts configured");
        return;
    }

    // --- JIT swap ----------------------------------------------------------
    const ModelParams& params = config_.expert(decision.expert);
    const bool already_resident = host_->loaded_expert() == decision.expert;

    // The display name, resolved once. Every status line below wants it, and a
    // seat ejected mid-turn would otherwise make each of them fall back to the
    // raw id independently.
    const std::string expert_name = expert_label(config_.roster, decision.expert);

    long load_ms = 0;
    if (!already_resident) {
        state_.set_resident(std::nullopt);
        state_.set_seat(decision.expert, SeatPhase::Loading, 0.0F);
        state_.set_mood(Mood::Loading, "swapping in " + expert_name);
        if (wake_) {
            wake_();
        }
    }

    const auto load_start = Clock::now();
    std::string error;
    LoadedModel* expert = host_->acquire_expert(
        decision.expert, params,
        [this, &decision](float progress) {
            state_.set_seat_progress(decision.expert, progress);
            if (wake_) {
                wake_();
            }
        },
        error);
    load_ms = already_resident ? 0 : ms_since(load_start);

    if (expert == nullptr) {
        state_.set_seat(decision.expert, SeatPhase::Dormant);
        state_.set_linked(std::nullopt);
        state_.fail_turn(turn, error);
        state_.set_mood(Mood::Error, error);
        return;
    }

    state_.set_resident(decision.expert);
    if (wake_) {
        wake_();
    }

    // --- generate ----------------------------------------------------------
    state_.set_mood(Mood::Thinking, expert_name + " is reading");
    if (wake_) {
        wake_();
    }

    std::vector<ChatMessage> messages;
    messages.push_back({"system", config_.system_prompt});
    if (!config_.reasoning_effort.empty()) {
        // Where a reasoning model looks for it. See Config::reasoning_effort.
        messages.front().content += "\n\nReasoning: " + config_.reasoning_effort;
    }
    if (config_.tools.web_search) {
        // Only when the tool is switched on. An expert told it can search when
        // it cannot will offer to, which is worse than not having the tool.
        messages.front().content += tools::tool_instructions();
    }

    // The same tools a cook gets, in an ordinary chat turn.
    //
    // There used to be a line here: cooking could change the project and
    // chatting could only talk about it. That was never a distinction anybody
    // asked for -- "fix the typo in README" is a sentence, not a goal worth
    // starting a cook for, and the answer to it is the edit. What separates the
    // two is how long they run and how they end, not what they may touch.
    //
    // The permission is the folder, answered once, in the terms it is about.
    const tools::WorkshopSettings workshop = workshop_for(project_root_);
    messages.front().content +=
        tools::workshop_instructions(workshop, tools::ToolAudience::Chat);
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t keep = kHistoryTurns * 2;
        const std::size_t from = history_.size() > keep ? history_.size() - keep : 0;
        messages.insert(messages.end(), history_.begin() + static_cast<long>(from),
                        history_.end());
    }
    messages.push_back({"user", request.prompt});

    // The live tok/s readout is measured from the first token rather than from
    // the start of the call: everything before that is prompt ingestion, and
    // folding it in would make a long prompt look like a slow expert.
    bool first_token = true;
    std::chrono::steady_clock::time_point first_token_at;
    int streamed_chunks = 0;

    GenerationStats stats;

    // One pass per round. Ordinarily there is exactly one: the expert answers
    // and that is the end of it. A round only repeats when the expert asked to
    // look something up, and the number of times it may do that is bounded --
    // an expert that reads results and searches again is being useful, one that
    // does it eight times is stuck. See tools/web_search.hpp.
    //
    // With the workshop the ceiling is higher and it is a different thing being
    // counted: reading a file, changing it and running the tests is three
    // rounds of honest work, where three searches is a model going in circles.
    int rounds = 1;
    if (config_.tools.web_search) {
        rounds = std::max(rounds, config_.tools.search_rounds + 1);
    }
    if (workshop.enabled) {
        rounds = std::max(rounds, kToolRounds);
    }
    std::vector<std::string> already_searched;
    for (int round = 0; round < rounds; ++round) {
        bool        first_answer = true;
        std::string answer;
        std::string reasoning;
        ResponseFilter filter;

        const GenerationStats pass = expert->generate(
            expert->format_chat(messages, true), params,
            [&](std::string_view raw) {
                if (first_token) {
                    first_token    = false;
                    first_token_at = std::chrono::steady_clock::now();
                    state_.set_mood(Mood::Thinking,
                                    expert_name + " is thinking");
                }
                // A reasoning model writes its working before its answer, and
                // on some of them the markers between the two are ordinary
                // visible text. See llm/response_filter.hpp.
                const ResponseFilter::Piece chunk = filter.feed(raw);
                if (!chunk.reasoning.empty()) {
                    reasoning += chunk.reasoning;
                    state_.append_reasoning(turn, chunk.reasoning);
                }
                if (!chunk.answer.empty()) {
                    if (first_answer) {
                        first_answer = false;
                        state_.set_mood(Mood::Talking,
                                        expert_name
                                            + " is answering");
                    }
                    answer += chunk.answer;
                    state_.append_reply(turn, chunk.answer);
                }

                // Recomputing the rate on every token would be noise on screen
                // and work in the hot path; a few times a second is what a
                // person can actually read.
                if (++streamed_chunks % 8 == 0) {
                    const double elapsed_s =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                      first_token_at).count();
                    if (elapsed_s > 0.05) {
                        state_.set_live_rate(static_cast<double>(streamed_chunks) / elapsed_s);
                    }
                }

                if (wake_) {
                    wake_();
                }
            },
            cancel);

        // Whatever was still held back, waiting to see if it was a marker.
        if (const ResponseFilter::Piece last = filter.flush();
            !last.answer.empty() || !last.reasoning.empty()) {
            state_.append_reasoning(turn, last.reasoning);
            state_.append_reply(turn, last.answer);
            reasoning += last.reasoning;
            answer    += last.answer;
        }

        stats.prompt_tokens += pass.prompt_tokens;
        stats.prompt_reused += pass.prompt_reused;
        stats.output_tokens += pass.output_tokens;
        stats.prompt_ms     += pass.prompt_ms;
        stats.output_ms     += pass.output_ms;
        stats.cancelled      = pass.cancelled;
        stats.hit_limit      = pass.hit_limit;

        if (pass.cancelled || round + 1 >= rounds) {
            break;
        }

        // --- did it reach for the project? ---------------------------------
        //
        // Checked before the search, because SEARCH is one of the verbs
        // parse_tool_call recognises and the search path below has bookkeeping
        // of its own -- rounds, repeats, the last-search warning -- that the
        // file verbs do not want.
        if (const std::optional<tools::ToolCall> call =
                workshop.enabled ? tools::parse_tool_call(answer, reasoning)
                                 : std::nullopt;
            call && is_project_verb(call->kind)) {
            // The verb and what it is being pointed at, which is the useful
            // half. Not "<expert> is <verb>ing": the verbs are nouns as much as
            // verbs here and half of them come out as "noteing" or "runing".
            std::string doing = std::string(tools::tool_kind_name(call->kind)) + " "
                              + call->argument;
            if (doing.size() > 64) {
                doing.resize(64);
                doing += "\u2026";
            }
            state_.set_mood(Mood::Thinking, doing);
            if (wake_) {
                wake_();
            }

            const tools::ToolResult result = tools::run_tool(
                *call, workshop, tools::SearchSettings{},
                [this] { return cancel_.load(std::memory_order_relaxed); });
            state_.add_action(turn, result.summary);

            // The call was a request, not an answer, and must not be left
            // sitting above the reply that replaces it.
            state_.set_reply(turn, {});
            messages.push_back({"assistant", answer});

            std::string handback = result.output;
            if (round + 2 >= rounds) {
                handback += "\n\nThat was the last action available this turn. Write "
                            "the answer now from what you have.";
            }
            messages.push_back({"user", std::move(handback)});
            if (wake_) {
                wake_();
            }
            continue;
        }

        const std::string query = tools::search_request(answer, reasoning);
        if (query.empty()) {
            break;  // it answered, which is the ordinary case
        }

        // --- the expert asked to look something up -------------------------
        //
        // Asking twice for the same thing is a model that has read the results
        // and not known what to do with them. Running the search again would
        // hand back the same page and invite it to do the same, so it is told
        // instead -- and this becomes its last round.
        const bool repeat = std::find(already_searched.begin(), already_searched.end(), query)
                            != already_searched.end();
        already_searched.push_back(query);

        state_.set_mood(Mood::Thinking,
                        repeat ? "already searched for \"" + query + "\""
                               : "searching for \"" + query + "\"");
        if (wake_) {
            wake_();
        }

        tools::SearchSettings settings;
        settings.enabled         = config_.tools.web_search;
        settings.provider        = config_.tools.search_provider;
        settings.endpoint        = config_.tools.search_endpoint;
        settings.api_key         = config_.tools.search_api_key;
        settings.max_results     = config_.tools.search_results;
        settings.timeout_seconds = config_.tools.search_timeout;

        std::string search_error;
        const std::vector<tools::SearchResult> results =
            repeat ? std::vector<tools::SearchResult>{}
                   : tools::search(query, settings, search_error);

        if (!repeat) {
            state_.add_action(turn, results.empty()
                                        ? "searched \"" + query + "\" -- " + search_error
                                        : "searched \"" + query + "\" -- "
                                              + std::to_string(results.size()) + " result"
                                              + (results.size() == 1 ? "" : "s") + " from "
                                              + settings.provider);
        }
        // That round produced a request, not an answer. The next round writes
        // the answer, and the request should not be sitting above it.
        state_.set_reply(turn, {});

        // Only the answer goes back, never the reasoning: the formats that
        // produce reasoning say to drop it from the context, and feeding it
        // back teaches the model that thinking aloud is part of the transcript.
        messages.push_back({"assistant", answer});
        std::string handback =
            repeat ? "You have already searched for \"" + query + "\" and been given the "
                     "results above."
                   : tools::format_for_model(query, results);
        if (repeat || round + 2 >= rounds) {
            // The last search it is allowed. Saying so is the difference
            // between an answer and a model that asks to search again and ends
            // the turn with nothing in it.
            handback += "\n\nThis was the last search available. Answer now from what you "
                        "have, and say plainly if it is not enough.";
        }
        messages.push_back({"user", std::move(handback)});
        if (wake_) {
            wake_();
        }
    }

    state_.finish_turn(turn, stats, load_ms);

    // Only remember exchanges that actually produced an answer, so a cancelled
    // turn -- or one that spent its whole budget thinking -- does not poison
    // the context of the next one. Read before the placeholder below is put in
    // its place: what goes into history has to be what the model said, not what
    // Crucible said about it.
    if (!stats.cancelled && stats.output_tokens > 0) {
        const Snapshot current = state_.snapshot();
        if (turn < current.turns.size() && !current.turns[turn].reply.empty()) {
            const std::lock_guard<std::mutex> lock(mutex_);
            history_.push_back({"user", request.prompt});
            history_.push_back({"assistant", current.turns[turn].reply});
        }
    }

    // A turn that ends without an answer in it.
    //
    // Three ways to get here, and they want different things said. A reasoning
    // model can spend its whole token budget in the channel the user does not
    // see and never reach the one they do; an expert with the search tool can
    // spend its last round asking to search again rather than answering; and a
    // raw "SEARCH: ..." line is the one thing that must never be shown as an
    // answer. Naming the wrong one sends the reader to the wrong setting.
    if (!stats.cancelled && stats.output_tokens > 0) {
        const Snapshot current = state_.snapshot();
        if (turn < current.turns.size()) {
            const Turn&        finished = current.turns[turn];
            const std::string& shown    = finished.reply;
            const bool asked_to_search  = !tools::search_request(shown, {}).empty();

            // A tool call left where the answer should be. It happens when the
            // last round the turn had was spent asking for one more action, and
            // showing "WRITE: src/main.cpp" as the reply is the one outcome
            // that must never reach the screen -- it reads as the expert having
            // said something.
            const std::optional<tools::ToolCall> unfinished =
                tools::parse_tool_call(shown, {});
            const bool asked_to_act = unfinished && is_project_verb(unfinished->kind);

            if (shown.empty() || asked_to_search || asked_to_act) {
                std::string why;
                if (asked_to_act) {
                    why = "the expert ran out of turns before it finished the work -- ask "
                          "again and it will carry on from what is on disk";
                } else if (asked_to_search) {
                    why = "the expert kept asking to search instead of answering -- raise "
                          "\"Search rounds\" in settings, or ask again more narrowly";
                } else if (stats.hit_limit) {
                    why = "the expert used its whole token budget thinking and never got to "
                          "an answer -- raise \"Max tokens\" in settings, or lower "
                          "\"Reasoning effort\" with /effort";
                } else {
                    why = "the expert stopped without writing an answer";
                }
                state_.set_reply(turn, "(" + why + ")");
            }
        }
    }

    // --- put the table back ------------------------------------------------
    //
    // With the delegator set to load on demand, the expert's turn is over the
    // moment it has answered, and holding it while nothing is happening is
    // holding memory the next decision needs. So the expert goes, the delegator
    // comes back, and the next prompt is routed the instant it arrives rather
    // than after a load.
    //
    // The cost is that a follow-up question reloads the expert. That is the
    // trade this mode is: one model resident at a time, so either of them may
    // be as large as the whole card.
    if (!config_.routing.keep_delegator_loaded) {
        if (host_->loaded_expert()) {
            host_->release_expert();
            state_.set_seat(decision.expert, SeatPhase::Dormant);
            state_.set_resident(std::nullopt);
        }
        if (host_->router() == nullptr && !config_.router.model.empty()) {
            state_.set_mood(Mood::Loading, "readying the delegator");
            if (wake_) {
                wake_();
            }
            ensure_router();
        }
    }

    // The work has stopped, so the line goes and the seat goes dark. Whether
    // the weights are still in memory is a separate question, and the status
    // bar is where it is answered.
    state_.set_linked(std::nullopt);
    state_.set_mood(Mood::Idle, stats.cancelled ? "cancelled" : "");
}

}  // namespace crucible
