// SPDX-License-Identifier: MIT
//
// The delegation loop.
//
// Everything llama.cpp touches happens on this thread. The UI hands over a
// prompt and learns how it went through AppState plus a wake callback -- it
// never calls into the engine's internals, and the engine never calls into
// the window toolkit.
//
// Config changes and expert releases ride the same queue as prompts, so they
// are applied in order and can never land in the middle of a generation.
#include "crucible/engine/engine.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>

#include "crucible/config/gpu_policy.hpp"
#include "crucible/config/paths.hpp"
#include "crucible/engine/route_policy.hpp"
#include "crucible/llm/response_filter.hpp"
#include "crucible/tools/web_search.hpp"
#include "crucible/engine/overflow.hpp"
#include "crucible/tools/workshop.hpp"

#include <fstream>
#include <sstream>
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
/// What the expert said before it reached for a tool.
///
/// A reply that ends in `WRITE: src/calc.py` and a fenced block is two things:
/// a sentence for the reader and a line for the machine. The line has to come
/// off the transcript -- shown as an answer it reads as the expert talking
/// about the protocol -- and the sentence has to stay, because it is the only
/// explanation of the change that will ever be written.
/// Two stretches of a reply, a paragraph apart -- or whichever one there is.
std::string paragraphs(const std::string& first, const std::string& second) {
    if (first.empty())  { return second; }
    if (second.empty()) { return first; }
    return first + "\n\n" + second;
}

std::string prose_before_tool_call(const std::string& answer, tools::ToolKind kind) {
    const std::string verb(tools::tool_kind_name(kind));
    std::string upper;
    for (const char c : verb) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    upper.push_back(':');

    // The first line that opens with the verb. Searched line by line rather
    // than with a bare find, so a mention of "WRITE:" inside the prose does not
    // truncate the very sentence being kept.
    std::size_t at = 0;
    while (at <= answer.size()) {
        std::size_t start = at;
        while (start < answer.size() && (answer[start] == ' ' || answer[start] == '\t')) {
            ++start;
        }
        if (answer.compare(start, upper.size(), upper) == 0) {
            std::string kept = answer.substr(0, at);
            while (!kept.empty() && (kept.back() == '\n' || kept.back() == ' ')) {
                kept.pop_back();
            }
            return kept;
        }
        const std::size_t end = answer.find('\n', at);
        if (end == std::string::npos) {
            break;
        }
        at = end + 1;
    }
    return {};   // nothing but the call: nothing to keep
}

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

/// How many past turns to replay to an expert, before the context is consulted.
///
/// Bounded deliberately: a swapped-in expert re-ingests the whole history from
/// cold, so an unbounded transcript would make every turn slower than the last.
/// This is a ceiling on work, not on meaning -- the overflow policy below is
/// what decides what actually fits.
constexpr std::size_t kHistoryTurns = 12;

/// The share of the context a conversation may occupy.
///
/// The rest is for the answer. A conversation trimmed to exactly the context
/// leaves the model no room to reply -- it ingests the prompt, has one token of
/// space, and stops -- which looks like the model refusing to answer rather
/// than like a context that is full.
constexpr double kPromptShare = 0.75;

}  // namespace

Engine::Engine(Config config, AppState& state, std::function<void()> wake)
    : config_(std::move(config)), state_(state), wake_(std::move(wake)) {
    auto_edits_.store(config_.tools.auto_edits);
}

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
    // A request to a provider is blocked reading the next line of the answer,
    // and would keep the worker -- and so the window closing -- waiting on it.
    hub_.interrupt();
    queued_.notify_all();
    edit_answered_.notify_all();
    cook_answered_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Engine::submit(std::string prompt, std::optional<ExpertId> pinned,
                    std::vector<attach::Attachment> attachments) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        Request request;
        request.kind        = RequestKind::Prompt;
        request.prompt      = std::move(prompt);
        request.pinned      = std::move(pinned);
        request.attachments = std::move(attachments);
        pending_.push_back(std::move(request));
    }
    queued_.notify_one();
}

attach::Composed Engine::read_attachments(std::vector<attach::Attachment> attachments,
                                          const ChatModel& model,
                                          const std::vector<ChatMessage>& messages,
                                          double share) {
    // A picture sent before keeps the bytes it was sent with; one the window
    // has just sent is remembered for the next time.
    for (attach::Attachment& one : attachments) {
        if (one.kind != attach::Kind::Image) {
            continue;
        }
        const auto known = std::find_if(pictures_.begin(), pictures_.end(),
                                        [&](const auto& entry) { return entry.first == one.path; });
        if (one.image.data.empty() && known != pictures_.end()) {
            one.image = known->second;
        } else if (!one.image.data.empty()) {
            if (known != pictures_.end()) {
                pictures_.erase(known);
            }
            pictures_.emplace_back(one.path, one.image);
            if (pictures_.size() > 16) {
                pictures_.erase(pictures_.begin());
            }
        }
    }

    // What is left of the share once the conversation is in it -- counting
    // no more than a quarter of the share for history, which the overflow
    // policy will trim to make room. A question about a document is about
    // the document more than about what was said three exchanges ago.
    std::vector<ChatMessage> bare;
    for (const ChatMessage& message : messages) {
        if (message.role == "system") {
            bare.push_back(message);
        }
    }
    if (!messages.empty()) {
        bare.push_back(messages.back());
    }
    const int budget  = static_cast<int>(static_cast<double>(model.context_size()) * share);
    const int base    = model.prompt_tokens(bare);
    const int history = std::max(0, model.prompt_tokens(messages) - base);
    const int room    = budget - base - std::min(history, budget / 4);
    // Three characters a token, which errs toward fitting for prose and is
    // about right for code; and never less than a page, so that a model
    // with a small context still sees the start of what it was given.
    const std::size_t chars = std::max<std::size_t>(static_cast<std::size_t>(std::max(room, 0)) * 3, 4000);
    return attach::compose(attachments, chars, model.sees_images());
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
    // Now, not when the queue reaches it: see auto_edits_.
    auto_edits_.store(config.tools.auto_edits);
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
    // A turn parked on an edit is inside await_edit_approval, not looking at
    // the flag. Waking it is what lets Stop end a turn that is waiting on you.
    edit_answered_.notify_all();
    // The same for a cook parked on a question. Missing this was a lockup:
    // Stop now on a cook that was asking something left the worker waiting
    // for an answer that would never come, and everything after it queued
    // behind a thread that would never take another request.
    cook_answered_.notify_all();
    // And one waiting on a provider is inside a read. A local model looks at
    // the flag between tokens; a remote one may not send a token for a minute
    // while it thinks, and Stop should not take a minute.
    hub_.interrupt();
}

void Engine::release_all() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        Request request;
        request.kind = RequestKind::ReleaseAll;
        pending_.push_back(std::move(request));
    }
    queued_.notify_one();
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

void Engine::approve_edit(bool approved) {
    {
        const std::lock_guard<std::mutex> lock(edit_mutex_);
        edit_approved_ = approved;
    }
    edit_answered_.notify_all();
}

bool Engine::await_edit_approval(std::size_t turn, const std::string& reply,
                                 const tools::ToolCall& call,
                                 const tools::WorkshopSettings& workshop) {
    // The protocol line comes off the transcript before the question goes up,
    // not after it is answered. Left on, the file is on screen twice while the
    // user decides -- once as a raw `WRITE: path` and a fence, once in the
    // panel asking about it -- and the raw one is the worse copy: no header, no
    // line numbers, and a language nobody declared.
    if (tools::resolve_in_root(workshop.root, call.argument)) {
        state_.set_reply(turn, reply);
    }
    return ask_about_edit(call, workshop);
}

bool Engine::ask_about_edit(const tools::ToolCall& call,
                            const tools::WorkshopSettings& workshop) {
    // What the file is now, so the two can be shown side by side. A path that
    // does not resolve inside the root is not a question to put to the user --
    // run_tool would refuse it anyway, and asking would be asking them to
    // approve something that cannot happen.
    const std::optional<std::filesystem::path> target =
        tools::resolve_in_root(workshop.root, call.argument);
    if (!target) {
        return true;   // let run_tool produce the refusal and its explanation
    }

    auto edit    = std::make_shared<PendingEdit>();
    edit->path   = call.argument;
    edit->after  = call.content;
    // As the write will leave it: the workshop ends every file with a
    // newline, and a preview without one shows a last line being removed
    // that nothing is going to remove.
    if (!edit->after.empty() && edit->after.back() != '\n') {
        edit->after += '\n';
    }
    if (std::ifstream in(*target, std::ios::binary); in) {
        std::ostringstream buffer;
        buffer << in.rdbuf();
        edit->before = buffer.str();
    }

    state_.set_pending_edit(edit);
    state_.set_mood(Mood::Idle, "waiting on you: " + call.argument);
    if (wake_) {
        wake_();
    }

    bool approved = false;
    {
        std::unique_lock<std::mutex> lock(edit_mutex_);
        edit_answered_.wait(lock, [this] {
            // Canceling and shutting down both release the wait. Without them
            // a turn parked on a question nobody is going to answer holds the
            // worker thread for the life of the process -- which is the exact
            // shape of the freeze that the loader used to have.
            return edit_approved_.has_value()
                || cancel_.load(std::memory_order_relaxed)
                || !running_.load(std::memory_order_relaxed);
        });
        approved = edit_approved_.value_or(false);
        edit_approved_.reset();
    }

    state_.set_pending_edit(nullptr);
    if (wake_) {
        wake_();
    }
    return approved;
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
                                "-- see Settings, Runtimes"
                              : "no runtime installed -- install one in Settings, Runtimes, "
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
        hub_.adopt(config_);
    }

    ready_delegator();
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
            router_failed_for_.clear();

            // The device list is exactly what just changed, and the split was
            // worked out from the old one.
            {
                const std::lock_guard<std::mutex> lock(config_mutex_);
                if (const std::string split = apply_gpu_policy(config_); !split.empty()) {
                    state_.add_notice("GPU split (" + config_.gpu.mode + "): " + split);
                }
                host_->set_gpu_config(config_.gpu);
            }

            ready_delegator();
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

        if (request.kind == RequestKind::ReleaseAll) {
            // The wrapper first, then the models. release_router drops the
            // ModelRouter that holds a reference to the loaded delegator, and
            // freeing the model out from under it would leave a live object
            // pointing at nothing.
            release_router();
            host_->release_expert();
            state_.set_resident(std::nullopt);
            state_.set_linked(std::nullopt);
            state_.set_mood(Mood::Idle, "nothing loaded");
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
            // take the process, and the window with it, down mid-edit.
            try {
                do_cook(request.prompt, request.budget_seconds, request.root, request.attachments);
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
            settle();
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
        // the process, so every request is contained: the turn fails, the
        // session survives.
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
        settle();
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

    state_.set_mood(Mood::Loading, "loading the delegator");
    state_.set_delegator_progress(0.0F);
    if (wake_) {
        wake_();
    }

    std::string error;
    LoadedModel* model = host_->acquire_router(
        config_.router,
        [this](float progress) {
            // A number of its own rather than a percentage written into the
            // status line: the panel draws it beside the delegator's name,
            // the same way it draws an expert's.
            state_.set_delegator_progress(progress);
            if (wake_) {
                wake_();
            }
        },
        // Closing the window ends a load as surely as Stop does.
        [this] {
            return cancel_.load(std::memory_order_relaxed)
                || !running_.load(std::memory_order_relaxed);
        },
        error);

    if (model == nullptr) {
        router_ = std::make_unique<KeywordRouter>(
            std::make_shared<const Roster>(config_.roster));
        // A load the user stopped is not a broken delegator, and should be
        // tried again the next time one is wanted.
        router_failed_for_ = error == "stopped" ? std::string() : config_.router.path;
        state_.add_notice("delegator: " + error + " -- routing on keywords instead");
        state_.set_delegator_ready(true);
        return;
    }

    auto routed = std::make_unique<ModelRouter>(
        *model, config_.router, std::make_shared<const Roster>(config_.roster));
    if (router_bias_for_ == config_.router.path) {
        routed->set_bias(router_bias_);
    }
    router_ = std::move(routed);
    router_failed_for_.clear();
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

void Engine::ready_delegator() {
    if (router_ && (config_.router.model.empty() || host_->router() != nullptr)) {
        return;  // already there, or keywords, which need nothing loaded
    }
    // A delegator that would not load the last time is not retried after
    // every prompt -- that is the same failure, and the same notice, on a
    // loop. A prompt still tries it (see ensure_router), and so does any
    // change to which file it is or what it runs on.
    if (router_ && router_failed_for_ == config_.router.path) {
        return;
    }
    load_router();
    // load_router leaves the status saying it is loading; the work is over.
    state_.set_mood(Mood::Idle);
}

void Engine::settle() {
    if (!running_.load(std::memory_order_relaxed)) {
        return;
    }
    // A Stop pressed during the request that just ended was about that
    // request. Left set, it would stop this load the moment it began, and the
    // next prompt would be routed on keywords for no reason anybody could see.
    cancel_.store(false, std::memory_order_relaxed);
    // Something else is already waiting. Loading the delegator for a prompt
    // that is queued is still right -- the prompt needs it first thing -- but
    // freeing an expert a queued cook or config change is about to use is not
    // worth deciding here, and the next request settles again when it ends.
    if (!config_.routing.keep_delegator_loaded) {
        if (const std::optional<ExpertId> resident = host_->loaded_expert()) {
            host_->release_expert();
            state_.set_seat(*resident, SeatPhase::Dormant);
            state_.set_resident(std::nullopt);
        }
    }
    const Snapshot before = state_.snapshot();
    ready_delegator();
    // Whatever the last request ended by saying -- "canceled", an error -- is
    // still the thing worth reading once the delegator is back.
    state_.set_mood(before.mood == Mood::Error ? Mood::Error : Mood::Idle, before.status);
    if (wake_) {
        wake_();
    }
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
        hub_.adopt(config_);
    }

    const Config current = this->config();

    // Drop the resident expert if the file behind its seat changed, so the next
    // prompt loads what the user just chose rather than the old weights.
    if (resident) {
        // An expert whose seat has been ejected outright reads as an empty
        // path here, which takes the same branch as one whose file changed:
        // drop it. That is what makes Eject free the weights of the
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
        ready_delegator();
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
    ready_delegator();
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
        // What was attached says something about who should answer: "what
        // does this do" means one thing beside main.rs and another beside
        // a lease agreement. The names are enough to say it.
        std::string routed = request.prompt;
        if (!request.attachments.empty()) {
            routed += "\n\nAttached:";
            for (const attach::Attachment& one : request.attachments) {
                routed += " " + (one.name.empty() ? std::filesystem::path(one.path).filename().string()
                                                  : one.name);
            }
        }
        decision = router_->route(routed, cancel);
    }
    // Then decide what to do about it.
    decision = apply_route_policy(decision, config_);

    if (!config_.routing.keep_delegator_loaded
        && !config_.expert(decision.expert).remote()) {
        // Its work for this prompt is done, and the expert is about to want
        // every byte it was holding. Unless the expert is somewhere else: then
        // nothing is about to be loaded, and freeing the delegator would only
        // mean loading it again for the next prompt.
        release_router();
    }
    return decision;
}

ChatModel* Engine::seat_model(const ExpertId& id, const ModelParams& params,
                              const std::string& name, long& load_ms, std::string& error) {
    load_ms = 0;
    if (params.remote()) {
        // Nothing to swap. Whatever is resident stays resident: it costs
        // nothing to leave, and the next prompt may well be for it.
        return hub_.model(params, error);
    }

    const bool already_resident = host_->loaded_expert() == id;
    if (!already_resident) {
        state_.set_resident(std::nullopt);
        state_.set_seat(id, SeatPhase::Loading, 0.0F);
        state_.set_mood(Mood::Loading, "swapping in " + name);
        if (wake_) {
            wake_();
        }
    }

    // acquire_expert frees whoever was resident before loading the next, which
    // is the whole memory argument for the design: the peak is the larger of
    // the two experts, never their sum.
    const auto load_start = Clock::now();
    LoadedModel* model = host_->acquire_expert(
        id, params,
        [this, &id](float progress) {
            state_.set_seat_progress(id, progress);
            if (wake_) {
                wake_();
            }
        },
        [this] { return cancel_.load(std::memory_order_relaxed); },
        error);
    if (model == nullptr) {
        state_.set_seat(id, SeatPhase::Dormant);
        return nullptr;
    }
    load_ms = already_resident ? 0 : ms_since(load_start);
    state_.set_resident(id);
    if (wake_) {
        wake_();
    }
    return model;
}

void Engine::handle(const Request& request) {
    std::vector<TurnAttachment> tiles;
    for (const attach::Attachment& one : request.attachments) {
        tiles.push_back(attach::tile_of(one));
    }
    const std::size_t turn = state_.begin_turn(request.prompt, std::move(tiles));

    const CancelCallback cancel = [this] { return cancel_.load(std::memory_order_relaxed); };

    // --- route -------------------------------------------------------------
    state_.set_mood(Mood::Routing, "Crucible is reading the prompt");
    if (wake_) {
        wake_();
    }

    const RouteDecision decision = resolve(request);
    state_.set_route(turn, decision);
    // From here the side menu draws a line from the delegator to this seat.
    state_.set_linked(decision.expert);
    if (wake_) {
        wake_();
    }

    if (cancel_.load(std::memory_order_relaxed)) {
        state_.fail_turn(turn, "canceled before routing finished");
        state_.set_linked(std::nullopt);
        state_.set_mood(Mood::Idle);
        return;
    }

    if (!config_.has_expert(decision.expert)) {
        state_.fail_turn(turn,
            "No expert has a model yet. Add one in Settings, Experts -- a GGUF file "
            "on this machine, or a model at a provider.");
        state_.set_linked(std::nullopt);
        state_.set_mood(Mood::Error, "no experts configured");
        return;
    }

    // --- JIT swap ----------------------------------------------------------
    const ModelParams& params = config_.expert(decision.expert);

    // The display name, resolved once. Every status line below wants it, and a
    // seat ejected mid-turn would otherwise make each of them fall back to the
    // raw id independently.
    const std::string expert_name = expert_label(config_.roster, decision.expert);

    long        load_ms = 0;
    std::string error;
    ChatModel*  expert = seat_model(decision.expert, params, expert_name, load_ms, error);

    if (expert == nullptr) {
        state_.set_linked(std::nullopt);
        // A load the user stopped is not an error to be explained, and the
        // turn it belonged to should read as canceled rather than failed.
        if (error == "stopped") {
            state_.cancel_turn(turn);
            state_.set_mood(Mood::Idle);
        } else {
            state_.fail_turn(turn, error);
            state_.set_mood(Mood::Error, error);
        }
        return;
    }

    // --- generate ----------------------------------------------------------
    state_.set_mood(Mood::Thinking, expert_name + " is reading");
    if (wake_) {
        wake_();
    }

    std::vector<ChatMessage> messages;
    messages.push_back({"system", config_.system_prompt});
    if (!config_.reasoning_effort.empty() && !params.remote()) {
        // Where a local reasoning model looks for it. See
        // Config::reasoning_effort. A provider takes it as a field of the
        // request instead, which is what ChatRequest::effort is for.
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
    // What was attached goes above what was typed, so the question comes
    // after the material it is about -- the order every model reads best.
    ChatMessage asked{"user", request.prompt};
    if (!request.attachments.empty()) {
        state_.set_mood(Mood::Thinking, expert_name + " is reading the attachments");
        if (wake_) {
            wake_();
        }
        messages.push_back(asked);
        attach::Composed composed = read_attachments(request.attachments, *expert, messages, kPromptShare);
        messages.pop_back();
        asked.content = composed.text + request.prompt;
        for (attach::Image& image : composed.images) {
            asked.images.push_back({std::move(image.mime), std::move(image.data)});
        }
    }
    messages.push_back(asked);

    // --- does it still fit? -------------------------------------------------
    //
    // Asked here, once the expert is loaded, because only the expert can answer
    // it: the context size is the one it was loaded with and the token count is
    // its own tokenizer's. Before this point there is no model to ask.
    {
        const Overflow policy = overflow_from_id(config_.tools.overflow);
        const int used   = expert->prompt_tokens(messages);
        const int budget = static_cast<int>(
            static_cast<double>(expert->context_size()) * kPromptShare);

        if (policy == Overflow::StopAtLimit && used > budget) {
            // Refused rather than shortened. The model cannot tell you what it
            // stopped being able to see, so the program says it instead.
            state_.set_linked(std::nullopt);
            state_.fail_turn(
                turn, "this conversation no longer fits in the context: "
                      + std::to_string(used) + " tokens of a "
                      + std::to_string(expert->context_size())
                      + "-token window. Raise \"Context size\" in settings, start a new "
                        "conversation, or change what happens on overflow.");
            state_.set_mood(Mood::Error, "context full");
            return;
        }
        const TokenCounter counter = [expert](const std::vector<ChatMessage>& what) {
            return expert->prompt_tokens(what);
        };
        if (const std::size_t dropped = trim_to_budget(policy, messages, counter, budget);
            dropped > 0) {
            state_.add_action(turn, TurnAction{
                "dropped " + std::to_string(dropped / 2)
                    + (dropped == 2 ? " earlier exchange" : " earlier exchanges")
                    + " to stay inside the context",
                {}, {}});
        }
        state_.set_context_used(expert->prompt_tokens(messages), expert->context_size());
    }

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

    // What the rounds before this one left on screen. A turn that reads a
    // file, then writes one, then answers says something each time, and each
    // is kept -- a paragraph apart, rather than run into the next sentence or
    // replaced by it.
    std::string earlier;
    for (int round = 0; round < rounds; ++round) {
        bool        first_answer = true;
        std::string answer;
        std::string reasoning;
        ResponseFilter filter;

        // The two things a piece of output can be, wherever it came from.
        const auto take = [&](std::string_view thought, std::string_view said) {
            if (first_token) {
                first_token    = false;
                first_token_at = std::chrono::steady_clock::now();
                state_.set_mood(Mood::Thinking, expert_name + " is thinking");
            }
            if (!thought.empty()) {
                reasoning += thought;
                state_.append_reasoning(turn, thought);
            }
            if (!said.empty()) {
                if (first_answer) {
                    first_answer = false;
                    state_.set_mood(Mood::Talking, expert_name + " is answering");
                    if (!earlier.empty()) {
                        state_.append_reply(turn, "\n\n");
                    }
                }
                answer += said;
                state_.append_reply(turn, said);
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
        };

        ChatSink sink;
        sink.cancel = cancel;
        if (expert->reasons_inline()) {
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
            expert->chat(ChatRequest{messages, params, config_.reasoning_effort}, sink);
        const GenerationStats& pass = outcome.stats;

        // Whatever was still held back, waiting to see if it was a marker.
        if (const ResponseFilter::Piece last = filter.flush();
            !last.answer.empty() || !last.reasoning.empty()) {
            state_.append_reasoning(turn, last.reasoning);
            state_.append_reply(turn, last.answer);
            reasoning += last.reasoning;
            answer    += last.answer;
        }

        // That a different model answered than the one in the seat, mostly.
        for (const std::string& note : outcome.notes) {
            state_.add_action(turn, TurnAction{note, {}, {}});
        }

        if (!outcome.error.empty()) {
            // The provider could not be asked, or stopped partway, or the
            // model declined. Only a remote model gets here: a local one that
            // is loaded answers.
            if (outcome.discard_partial) {
                // What streamed before a refusal is not half an answer.
                state_.set_reply(turn, earlier);
            } else if (!answer.empty()) {
                // fail_turn keeps a reply that had started, which is right --
                // but then nothing would say why it stops where it does.
                state_.add_action(turn, TurnAction{"cut short: " + outcome.error, {}, {}});
            }
            state_.fail_turn(turn, outcome.error);
            state_.set_linked(std::nullopt);
            state_.set_mood(Mood::Error, outcome.error);
            if (wake_) {
                wake_();
            }
            return;
        }

        stats.prompt_tokens += pass.prompt_tokens;
        stats.prompt_reused += pass.prompt_reused;
        stats.output_tokens += pass.output_tokens;
        stats.prompt_ms     += pass.prompt_ms;
        stats.output_ms     += pass.output_ms;
        stats.canceled      = pass.canceled;
        stats.hit_limit      = pass.hit_limit;

        if (pass.canceled || round + 1 >= rounds) {
            break;
        }

        // --- did it reach for the project? ---------------------------------
        //
        // Checked before the search, because SEARCH is one of the verbs
        // parse_tool_call recognizes and the search path below has bookkeeping
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

            // --- the gate ---------------------------------------------
            //
            // A write is the one tool call that changes something the user owns,
            // so it is the one that stops and asks -- unless they have said not
            // to. Everything else (reading, listing, running) either changes
            // nothing or was already agreed to by trusting the folder.
            // The turn so far, and this round's prose up to the call.
            const std::string kept = paragraphs(earlier, prose_before_tool_call(answer, call->kind));
            if (call->kind == tools::ToolKind::Write && !auto_edits_.load()
                && !await_edit_approval(turn, kept, *call, workshop)) {
                state_.add_action(turn, TurnAction{
                    "declined the edit to " + call->argument, {}, {}});
                // This round described a change that did not happen; what
                // came before it still stands.
                state_.set_reply(turn, earlier);
                messages.push_back({"assistant", answer});
                messages.push_back(
                    {"user", "The user declined that edit; the file is unchanged. Do "
                             "not try the same write again. Either explain what you "
                             "were going to change and why, or propose something "
                             "different."});
                if (wake_) {
                    wake_();
                }
                continue;
            }

            const tools::ToolResult result = tools::run_tool(
                *call, workshop, tools::SearchSettings{},
                [this] { return cancel_.load(std::memory_order_relaxed); });
            // The diff a write made, or the output a command printed, kept for
            // the transcript rather than only handed to the model.
            state_.add_action(turn, TurnAction{result.summary, result.detail,
                                               call->kind == tools::ToolKind::Write
                                                   ? call->argument
                                                   : std::string()});

            // The protocol line goes and the prose around it stays.
            //
            // This used to clear the whole reply, on the grounds that the call
            // was a request rather than an answer. True of the `WRITE: path`
            // line; false of the sentence above it explaining what was about to
            // change, which was thrown away with it every time.
            earlier = kept;
            state_.set_reply(turn, earlier);
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
            state_.add_action(turn, TurnAction{
                results.empty()
                    ? "searched \"" + query + "\" -- " + search_error
                    : "searched \"" + query + "\" -- "
                          + std::to_string(results.size()) + " result"
                          + (results.size() == 1 ? "" : "s") + " from " + settings.provider,
                {}, {}});
        }
        // That round produced a request, not an answer. The next round writes
        // the answer, and the request should not be sitting above it.
        state_.set_reply(turn, earlier);

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

    // Only remember exchanges that actually produced an answer, so a canceled
    // turn -- or one that spent its whole budget thinking -- does not poison
    // the context of the next one. Read before the placeholder below is put in
    // its place: what goes into history has to be what the model said, not what
    // Crucible said about it.
    if (!stats.canceled && stats.output_tokens > 0) {
        const Snapshot current = state_.snapshot();
        if (turn < current.turns.size() && !current.turns[turn].reply.empty()) {
            const std::lock_guard<std::mutex> lock(mutex_);
            // With what was attached, so that the next question can be about
            // it too. It is the first thing trimmed when room runs short.
            history_.push_back(asked);
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
    if (!stats.canceled && stats.output_tokens > 0) {
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
                          "\"Reasoning effort\" in settings";
                } else {
                    why = "the expert stopped without writing an answer";
                }
                state_.set_reply(turn, "(" + why + ")");
            }
        }
    }

    // The table is put back by settle(), once this returns and the engine is
    // no longer busy -- for this ending and for every early return above.

    // The work has stopped, so the line goes and the seat goes dark. Whether
    // the weights are still in memory is a separate question, and the status
    // bar is where it is answered.
    state_.set_linked(std::nullopt);
    state_.set_mood(Mood::Idle, stats.canceled ? "canceled" : "");
}

}  // namespace crucible
