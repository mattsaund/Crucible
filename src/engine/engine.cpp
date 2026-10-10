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
#include <stdexcept>
#include <thread>

#include "crucible/config/gpu_policy.hpp"
#include "crucible/config/paths.hpp"
#include "crucible/lab/python.hpp"
#include "crucible/llm/response_filter.hpp"
#include "crucible/tools/web_search.hpp"
#include "crucible/engine/overflow.hpp"
#include "crucible/tools/workshop.hpp"

#include <fstream>
#include <sstream>
#include "crucible/runtime/registry.hpp"
#include "crucible/session/store.hpp"
#include "crucible/util/format.hpp"

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
        case tools::ToolKind::Edit:
        case tools::ToolKind::Find:
        case tools::ToolKind::Render:
        case tools::ToolKind::Mcp:
        case tools::ToolKind::Run:
        case tools::ToolKind::Note:
        case tools::ToolKind::Fetch:
        case tools::ToolKind::Git:
        case tools::ToolKind::Gh:
        case tools::ToolKind::Python:
        case tools::ToolKind::Start:
        case tools::ToolKind::Stop:
        case tools::ToolKind::Logs:
        case tools::ToolKind::Screenshot:
        case tools::ToolKind::Click:
        case tools::ToolKind::Move:
        case tools::ToolKind::Type:
        case tools::ToolKind::Key:
        case tools::ToolKind::Scroll:
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
    : config_(std::move(config)), state_(state), wake_(std::move(wake)),
      hub_(paths::data_dir() / "spend.json") {
    auto_edits_.store(config_.tools.auto_edits);
    // What the orchestrator asks for, answered on the link's own threads --
    // the chat's routing and a build's agents side by side.
    orchestra_.set_handler([this](const std::string& method, const nlohmann::json& params) {
        return serve(method, params);
    });
}

Engine::~Engine() {
    stop();
}

void Engine::start() {
    if (running_.exchange(true)) {
        return;
    }
    worker_ = std::thread(&Engine::run, this);
    warm_mcp(config());
}

void Engine::warm_mcp(const Config& config) {
    const std::lock_guard<std::mutex> lock(mcp_mutex_);
    std::vector<tools::mcp::ServerConfig> servers;
    for (const McpServer& one : config.tools.mcp) {
        tools::mcp::ServerConfig server;
        server.name    = one.name;
        server.command = one.command;
        server.args    = one.args;
        server.enabled = one.enabled;
        for (const auto& [key, value] : one.env) {
            server.env.push_back(key + "=" + value);
        }
        servers.push_back(std::move(server));
    }
    mcp_.configure(servers);
    // One warm-up at a time; the last configuration is the one that counts,
    // and starting a server twice would be two of it.
    if (mcp_thread_.joinable()) {
        mcp_thread_.join();
    }
    if (mcp_.any()) {
        mcp_thread_ = std::thread([this] { mcp_.warm(); });
    }
}

void Engine::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    cancel_.store(true, std::memory_order_relaxed);
    cook_stop_.store(true, std::memory_order_relaxed);
    cook_cancel_.store(true, std::memory_order_relaxed);
    // A request to a provider is blocked reading the next line of the answer,
    // and would keep the worker -- and so the window closing -- waiting on it.
    hub_.interrupt();
    queued_.notify_all();
    edit_answered_.notify_all();
    cook_answered_.notify_all();
    leases_.wake_all();

    // The build first: its agents hold models the worker is about to free.
    // Every wait it can be in has just been woken and told to stop, so it
    // ends within a round -- unless the orchestrator itself has hung, which
    // is what stopping the orchestrator after a grace period is for: its
    // pipe closing ends the call the build thread is blocked in.
    if (cook_thread_.joinable()) {
        for (int waited = 0; waited < 50 && cooking_.load(std::memory_order_relaxed); ++waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (cooking_.load(std::memory_order_relaxed)) {
            orchestra_.stop();
        }
        cook_thread_.join();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    // Nothing is left running behind a window that is gone: not what experts
    // started, and not the MCP servers.
    processes_.stop_all();
    {
        const std::lock_guard<std::mutex> lock(mcp_mutex_);
        if (mcp_thread_.joinable()) {
            mcp_thread_.join();
        }
    }
    mcp_.stop_all();
}

std::vector<Engine::MadeSeat> Engine::take_made_seats() {
    const std::lock_guard<std::mutex> lock(written_mutex_);
    return std::exchange(made_seats_, {});
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
    // has just sent is remembered for the next time. Under a lock: a build's
    // agent reads the directive's pictures while a chat turn reads its own.
    std::unique_lock<std::mutex> remembered(pictures_mutex_);
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
    remembered.unlock();

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

void Engine::name_session(std::string session, std::filesystem::path root, std::string excerpt) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        Request request;
        request.kind    = RequestKind::NameSession;
        request.session = std::move(session);
        request.root    = std::move(root);
        request.prompt  = std::move(excerpt);
        pending_.push_back(std::move(request));
    }
    queued_.notify_one();
}

std::vector<Engine::SessionName> Engine::take_session_names() {
    const std::lock_guard<std::mutex> lock(written_mutex_);
    return std::exchange(session_names_, {});
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
    // One waiting for a model another seat is holding is in a wait of its
    // own, and looks at the flag when woken.
    leases_.wake_all();
    // And one waiting on a provider is inside a read. A local model looks at
    // the flag between tokens; a remote one may not send a token for a minute
    // while it thinks, and Stop should not take a minute. Only the requests
    // whose switch is this one are cut: a build's agents carry on.
    hub_.interrupt();
}

ModelHost::Keep Engine::in_use() const {
    return [this](const std::string& path) { return leases_.held(path); };
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
    return ask_about_edit(call, workshop, [this] { return cancel_.load(std::memory_order_relaxed); });
}

bool Engine::ask_about_edit(const tools::ToolCall& call,
                            const tools::WorkshopSettings& workshop,
                            const CancelCallback& cancel) {
    // What the file is now, so the two can be shown side by side. A path that
    // does not resolve inside the root is not a question to put to the user --
    // run_tool would refuse it anyway, and asking would be asking them to
    // approve something that cannot happen.
    const std::optional<std::filesystem::path> target =
        tools::resolve_in_root(workshop.root, call.argument);
    if (!target) {
        return true;   // let run_tool produce the refusal and its explanation
    }

    // One edit on screen at a time: an agent of the build and the chat may
    // both want a yes, and the window asks about one file at once.
    const std::lock_guard<std::mutex> one_at_a_time(asking_mutex_);

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
        edit_answered_.wait(lock, [this, &cancel] {
            // Canceling and shutting down both release the wait. Without them
            // a turn parked on a question nobody is going to answer holds the
            // worker thread for the life of the process -- which is the exact
            // shape of the freeze that the loader used to have.
            return edit_approved_.has_value() || (cancel && cancel())
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

void Engine::stop_mlx() {
    mlx_.stop();
    if (mlx_seat_) {
        state_.set_seat(*mlx_seat_, SeatPhase::Dormant);
        if (state_.snapshot().resident == mlx_seat_) {
            state_.set_resident(std::nullopt);
        }
        mlx_seat_.reset();
    }
    show_loaded();
}

void Engine::show_loaded() {
    const auto name = [](const std::string& path) { return std::filesystem::path(path).filename().string(); };
    std::vector<LocalModel> models;
    if (host_) {
        if (const LoadedModel* router = host_->router(); router != nullptr) {
            models.push_back({{}, name(router->path()), router->bytes(), true});
        }
        for (const ModelHost::Held& held : host_->held()) {
            models.push_back({held.id, name(held.model->path()), held.model->bytes(), false});
        }
    }
    if (mlx_seat_ && mlx_.running()) {
        // A folder of weights in a process of its own: what it holds is
        // about what the folder does.
        const std::filesystem::path served = mlx_.served();
        std::uint64_t bytes = 0;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(served, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code each;
            if (it->is_regular_file(each)) {
                bytes += it->file_size(each);
            }
        }
        models.push_back({*mlx_seat_, served.filename().string(), bytes, false});
    }
    state_.set_local_models(std::move(models));
}

void Engine::restore_history(std::vector<ChatMessage> history) {
    const std::lock_guard<std::mutex> lock(mutex_);
    history_ = std::move(history);
}

void Engine::run() {
    // Constructing the host loads the runtimes, so nothing before this point
    // knows what hardware exists -- which is why the GPU policy is applied
    // here rather than when the config was parsed.
    {
        const std::lock_guard<std::mutex> host(host_mutex_);
        host_ = std::make_unique<ModelHost>(paths::log_file());
    }
    host_ready_.store(true, std::memory_order_release);

    // Which cards there are, and how a model would be split across them, is
    // Settings, Hardware's to say. The transcript is for the conversation; a
    // list of devices at the top of every one is not part of it.
    const std::vector<std::string> devices = ModelHost::devices();
    // Said at startup rather than at the first prompt: with no runtime there
    // is no hardware to run a model on, and finding that out only when you
    // have typed a question is the worse way to learn it.
    if (devices.empty()) {
        state_.add_notice(RuntimeRegistry::any_installed()
                              ? "the runtime found no hardware -- see Settings, Runtimes"
                              : "no runtime -- install one in Settings, Runtimes");
    }

    {
        const std::lock_guard<std::mutex> lock(config_mutex_);
        apply_gpu_policy(config_);
        // Every load re-plans its own split from live memory, and the host is
        // where that happens. See refresh_gpu_split.
        host_->set_gpu_config(config_.gpu);
        hub_.adopt(config_);
    }

    // Nothing is loaded at startup. The delegator comes when the first prompt
    // needs it, and after each prompt it is brought back for the next one
    // (see settle) -- but a window that was only opened costs no memory on a
    // card somebody may be using for something else.
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
            // the moment least likely to have room for it. A model a build's
            // agent is using stays until it lets go, and loads the new way
            // the next time it is wanted.
            {
                const std::lock_guard<std::mutex> host(host_mutex_);
                host_->release_experts(in_use());
                state_.set_resident(host_->loaded_expert());
                show_loaded();
                release_router();
                router_failed_for_.clear();

                // The device list is exactly what just changed, and the split
                // was worked out from the old one.
                const std::lock_guard<std::mutex> lock(config_mutex_);
                apply_gpu_policy(config_);
                host_->set_gpu_config(config_.gpu);
            }

            // Not loaded again here: the next prompt loads it, on the new
            // hardware.
            state_.set_mood(Mood::Idle, "runtime changed");
            if (wake_) {
                wake_();
            }
            continue;
        }

        if (request.kind == RequestKind::ReleaseExpert || request.kind == RequestKind::ReleaseAll) {
            // Eject frees what nobody is using. What a build's agent holds
            // goes when the agent lets go of it, which the status line says,
            // rather than out from under a sentence it is writing.
            const bool all = request.kind == RequestKind::ReleaseAll;
            bool kept = false;
            {
                const std::lock_guard<std::mutex> host(host_mutex_);
                if (all) {
                    release_router();
                }
                host_->release_experts(in_use());
                if (!leases_.held(mlx_.served().string())) {
                    stop_mlx();
                }
                kept = host_->expert_count() > 0 || mlx_.running();
                state_.set_resident(host_->loaded_expert());
                show_loaded();
            }
            if (all && !kept) {
                state_.set_linked(std::nullopt);
            }
            state_.set_mood(Mood::Idle, kept ? "released what is not in use -- the build is using the rest"
                                      : all ? "nothing loaded" : "expert released");
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

        if (request.kind == RequestKind::NameSession) {
            try {
                do_name_session(request);
            } catch (...) {
                // A conversation without a name is listed by its first prompt,
                // which is what it was before names existed.
            }
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

    // A build still finishing holds models; they are freed once it has let
    // go of them. stop() has already told it to stop.
    while (cooking_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Free the models before the backend goes away, and stop MLX's server and
    // the orchestrator, which are processes of their own and would outlive
    // the window.
    {
        const std::lock_guard<std::mutex> host(host_mutex_);
        host_.reset();
        stop_mlx();
    }
    orchestra_.stop();
}

void Engine::load_router() {
    // A copy: the build's thread may be adding a seat to the config as this
    // runs, and the delegator's settings are what is read here.
    const ModelParams router = config().router;
    if (router.model.empty()) {
        // The side menu says "(none)" on the delegator's row, and why.
        state_.set_delegator_problem({});
        state_.set_delegator_ready(true);  // keywords need nothing loaded
        return;
    }

    state_.set_mood(Mood::Loading, "loading the delegator");
    state_.set_delegator_progress(0.0F);
    if (wake_) {
        wake_();
    }

    std::string error;
    bool        crowded = false;
    LoadedModel* model = host_->acquire_router(
        router,
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
        error, in_use(), &crowded);

    if (model == nullptr && crowded) {
        // Not a broken delegator: the experts a build is using leave it no
        // room for now. Prompts go on keywords until they are let go, and it
        // is tried again then -- so nothing is remembered as a failure.
        state_.set_delegator_problem({});
        state_.set_delegator_ready(true);
        return;
    }
    if (model == nullptr) {
        // A load the user stopped is not a broken delegator, and should be
        // tried again the next time one is wanted. Until it loads, prompts
        // are routed on keywords -- the orchestrator is told it is not there.
        router_failed_for_ = error == "stopped" ? std::string() : router.path;
        // On the delegator's row in the side menu, not in the chat.
        state_.set_delegator_problem(error == "stopped" ? std::string() : error);
        state_.set_delegator_ready(true);
        return;
    }
    router_failed_for_.clear();
    state_.set_delegator_problem({});
    state_.set_delegator_ready(true);
    show_loaded();
}

void Engine::ensure_router() {
    if (config().router.model.empty() || host_->router() != nullptr) {
        return;  // keywords, which need nothing loaded, or already there
    }
    load_router();
}

void Engine::ready_delegator() {
    const ModelParams router = config().router;
    if (router.model.empty()) {
        load_router();   // nothing to load; says so on the delegator's row
        return;
    }
    if (host_->router() != nullptr) {
        return;  // already there
    }
    // A delegator that would not load the last time is not retried after
    // every prompt -- that is the same failure, on a loop. A prompt still
    // tries it (see ensure_router), and so does any change to which file it
    // is or what it runs on.
    if (router_failed_for_ == router.path) {
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
    tidy_models();
}

void Engine::tidy_models() {
    if (!running_.load(std::memory_order_relaxed)) {
        return;
    }
    const Snapshot before = state_.snapshot();
    {
        const std::lock_guard<std::mutex> host(host_mutex_);
        // On demand: the experts nobody is using go, so the delegator has
        // the room. One a build's agent holds stays -- it is mid-task.
        if (!config().routing.keep_delegator_loaded) {
            const std::optional<ExpertId> was = host_->loaded_expert();
            host_->release_experts(in_use());
            if (was && host_->loaded_expert() != was) {
                state_.set_seat(*was, SeatPhase::Dormant);
            }
            state_.set_resident(host_->loaded_expert());
            show_loaded();
            if (!leases_.held(mlx_.served().string())) {
                stop_mlx();
            }
        }
        ready_delegator();
    }
    // Whatever the last request ended by saying -- "canceled", an error -- is
    // still the thing worth reading once the delegator is back.
    state_.set_mood(before.mood == Mood::Error ? Mood::Error : Mood::Idle, before.status);
    if (wake_) {
        wake_();
    }
}

void Engine::release_router() {
    // The orchestrator keeps the delegator's measured lean itself, by file,
    // so freeing the model costs the next load nothing but the load.
    host_->release_router();
    state_.set_delegator_ready(false);
    show_loaded();
}

namespace {

/// Whether a model loaded with `before` has to be loaded again to be `after`:
/// the settings baked in when it loads -- the file, its context, how it sits
/// on the cards -- not the ones handed to each request.
bool loads_differently(const ModelParams& before, const ModelParams& after) {
    return before.path != after.path || before.n_ctx != after.n_ctx
        || before.n_batch != after.n_batch || before.n_gpu_layers != after.n_gpu_layers
        || before.split_mode != after.split_mode || before.flash_attn != after.flash_attn;
}

/// Whether Settings, Hardware changed anything.
bool hardware_differs(const GpuConfig& before, const GpuConfig& after) {
    return before.mode != after.mode || before.priority != after.priority
        || before.main_gpu != after.main_gpu || before.gpu_only != after.gpu_only
        || before.vram_only != after.vram_only;
}

}  // namespace

void Engine::do_apply_config(Config config) {
    config.resolve_models();
    apply_gpu_policy(config);

    const std::lock_guard<std::mutex> host(host_mutex_);
    Config before;
    const std::optional<ExpertId> resident = host_->loaded_expert();
    {
        const std::lock_guard<std::mutex> lock(config_mutex_);
        before  = config_;
        config_ = std::move(config);
        host_->set_gpu_config(config_.gpu);
        hub_.adopt(config_);
    }
    const Config current = this->config();
    warm_mcp(current);

    // A loaded model keeps the cards and the context it was loaded with, so a
    // change to either -- in Settings, Hardware, or a seat's own loading
    // settings -- does nothing until it is loaded again. It is freed here and
    // the next prompt loads it the new way; otherwise a change would only
    // take effect after an Eject or a restart, which reads as one that did not
    // stick.
    const bool hardware = hardware_differs(before.gpu, current.gpu);
    bool       freed    = false;

    // The resident expert, if its seat now loads differently -- or was ejected
    // outright, which reads as an empty path and is the same answer: drop it,
    // rather than leave its weights resident and unreachable. Only what
    // nobody is using: a build's agent keeps the model it has until it lets
    // go, and the next seat to want it loads it the new way.
    if (resident) {
        const ModelParams now = current.expert(*resident);
        if (now.path.empty() || hardware || loads_differently(before.expert(*resident), now)) {
            host_->release_experts(in_use());
            if (host_->loaded_expert() != resident) {
                state_.set_seat(*resident, SeatPhase::Dormant);
            }
            state_.set_resident(host_->loaded_expert());
            show_loaded();
            freed = true;
        }
    }

    // The delegator, likewise; a different one is loaded by the next prompt
    // that needs it.
    if (host_->router() != nullptr
        && (hardware || loads_differently(before.router, current.router))) {
        release_router();
        freed = true;
    }
    if (before.router.path != current.router.path) {
        router_failed_for_.clear();
    }

    state_.configure_seats(current);
    state_.set_resident(host_->loaded_expert());
    show_loaded();
    state_.set_mood(Mood::Idle, freed ? "settings applied -- models load with them from the next prompt"
                                      : "settings applied");
}

void Engine::do_name_session(const Request& request) {
    // The orchestrator's: it asks the delegator, when there is one, and takes
    // the opening words of the first prompt when there is not. Without the
    // orchestrator there is no name, and the list shows that first prompt,
    // which is what it showed before there were names.
    std::string error;
    if (!wait_for_python(error)) {
        return;
    }
    std::string name;
    try {
        const nlohmann::json out = orchestra_.call(
            "name.session", {{"excerpt", request.prompt}, {"context", "chat"}});
        name = out.value("name", "");
    } catch (const std::exception&) {
        return;
    }
    if (name.empty()) {
        return;
    }
    const std::lock_guard<std::mutex> lock(written_mutex_);
    session_names_.push_back({request.session, request.root, std::move(name)});
}

void Engine::do_write_examples(const ExpertId& id) {
    const Config config = this->config();
    const std::optional<std::size_t> seat = config.roster.find(id);
    if (!seat) {
        return;  // ejected again before this ran, which is a perfectly good answer
    }
    const Expert expert = config.roster.at(*seat);
    if (!expert.examples.empty()) {
        return;  // already has them; this is not a rewrite
    }
    std::string error;
    if (!wait_for_python(error)) {
        return;  // the seat still routes on its blurb and keywords
    }

    state_.set_mood(Mood::Thinking, "writing examples for " + expert.name);
    if (wake_) {
        wake_();
    }
    std::vector<std::string> examples;
    try {
        const nlohmann::json out = orchestra_.call(
            "examples.write", {{"name", expert.name}, {"blurb", expert.blurb}, {"context", "chat"}});
        for (const nlohmann::json& one : out.value("examples", nlohmann::json::array())) {
            if (one.is_string()) {
                examples.push_back(one.get<std::string>());
            }
        }
    } catch (const std::exception&) {
        examples.clear();
    }
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
    // Folded into the engine's own copy as well, so the next prompt is routed
    // with them: the window's copy is updated when it drains the outbox, and
    // waiting for that round trip would route the first prompt without them.
    Expert updated = expert;
    updated.examples = std::move(examples);
    {
        const std::lock_guard<std::mutex> lock(config_mutex_);
        config_.roster.update(id, updated);
    }
}

bool Engine::wait_for_python(std::string& error) {
    if (lab::python::installed()) {
        return true;
    }
    // Being fetched as Crucible starts (see the window's setup): a prompt sent
    // in the first minute of a new install waits for it rather than failing.
    if (lab::python::installing()) {
        state_.set_mood(Mood::Loading, "getting ready: downloading Crucible's Python");
        if (wake_) {
            wake_();
        }
        while (lab::python::installing() && !lab::python::installed()) {
            if (cancel_.load(std::memory_order_relaxed) || !running_.load(std::memory_order_relaxed)) {
                error = "stopped";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (lab::python::installed()) {
            return true;
        }
    }
    error = "Routing and cooks run on Crucible's own Python, which is not installed. It is "
            "downloaded when Crucible starts -- check the connection and restart Crucible, "
            "or run crucible --install-python.";
    return false;
}

nlohmann::json Engine::routing_request(const std::string& prompt,
                                       const std::optional<ExpertId>& pinned) const {
    using json = nlohmann::json;
    const Config config = this->config();
    json roster = json::array();
    json seats  = json::object();
    for (const Expert& expert : config.roster.experts()) {
        roster.push_back(json{{"id", expert.id}, {"name", expert.name}, {"tag", expert.tag},
                              {"blurb", expert.blurb}, {"keywords", expert.keywords},
                              {"examples", expert.examples}});
        const ModelParams params = config.expert(expert.id);
        // Which model on this machine answers for the seat, when one does: a
        // build runs one agent at a time on each, since two agents on one
        // model take turns at it and throw away each other's cache.
        seats[expert.id] = json{{"model", config.has_expert(expert.id)},
                                {"remote", params.remote()},
                                {"local", params.remote() ? std::string() : params.path}};
    }
    return json{
        {"prompt", prompt},
        {"pinned", pinned ? *pinned : std::string()},
        {"roster", roster},
        {"seats", seats},
        {"routing", {{"min_confidence", config.routing.min_confidence},
                     {"default_expert", config.routing.default_expert}}},
    };
}

CancelCallback Engine::cancel_for(const nlohmann::json& params) const {
    if (params.value("context", std::string()) == "build") {
        return [this] {
            return cook_cancel_.load(std::memory_order_relaxed) || !running_.load(std::memory_order_relaxed);
        };
    }
    return [this] {
        return cancel_.load(std::memory_order_relaxed) || !running_.load(std::memory_order_relaxed);
    };
}

nlohmann::json Engine::serve(const std::string& method, const nlohmann::json& params) {
    using json = nlohmann::json;
    const bool           building = params.value("context", std::string()) == "build";
    const CancelCallback cancel   = cancel_for(params);

    // --- the delegator -----------------------------------------------------
    //
    // Under the host's lock throughout. With the delegator on demand it is
    // freed for an expert, and a score halfway through it would be reading
    // memory that had gone; a score takes a fraction of a second, and a load
    // waiting that long behind it costs nothing anyone can see.
    if (method == "delegator.ready") {
        const std::lock_guard<std::mutex> host(host_mutex_);
        ensure_router();
        const bool loaded = host_->router() != nullptr;
        return json{{"available", loaded}, {"path", loaded ? config().router.path : std::string()}};
    }
    if (method == "delegator.generate") {
        // A few words of writing -- a conversation's name, an expert's
        // examples. A reasoning model thinks first: its thinking is filtered
        // out, it is given room for it, and asked for as little of it as it
        // takes.
        const std::lock_guard<std::mutex> host(host_mutex_);
        LoadedModel* model = host_->router();
        if (model == nullptr) {
            throw std::runtime_error("the delegator is not loaded");
        }
        std::vector<ChatMessage> messages;
        if (model->takes_effort()) {
            messages.push_back({"system", "Reasoning: low"});
        }
        for (const json& one : params.value("messages", json::array())) {
            messages.push_back({one.value("role", "user"), one.value("content", "")});
        }
        ModelParams asked = config().router;
        asked.temperature = params.value("temperature", 0.2F);
        asked.max_tokens  = params.value("max_tokens", 64);
        if (model->takes_effort()) {
            asked.max_tokens = std::max(asked.max_tokens, 512);   // its thinking comes first
        }
        std::string    reply;
        ResponseFilter filter;   // a thinking block or a harmony channel is not the answer
        model->generate(model->format_chat(messages, true), asked,
                        [&](std::string_view chunk) { reply += filter.feed(chunk).answer; },
                        cancel);
        reply += filter.flush().answer;
        return json{{"text", reply}};
    }
    if (method == "delegator.format" || method == "delegator.score") {
        const std::lock_guard<std::mutex> host(host_mutex_);
        LoadedModel* model = host_->router();
        if (model == nullptr) {
            throw std::runtime_error("the delegator is not loaded");
        }
        if (method == "delegator.format") {
            std::vector<ChatMessage> messages;
            for (const json& one : params.value("messages", json::array())) {
                messages.push_back({one.value("role", "user"), one.value("content", "")});
            }
            return json{{"text", model->format_chat(messages, true)}};
        }
        std::vector<std::string> labels;
        for (const json& one : params.value("labels", json::array())) {
            labels.push_back(one.is_string() ? one.get<std::string>() : std::string());
        }
        const bool cancelable = params.value("cancelable", true);
        const std::vector<float> scores =
            model->score_labels(params.value("prompt", ""), labels, cancelable ? cancel : CancelCallback{});
        json out = json::array();
        for (const float score : scores) {
            out.push_back(score <= kUnscored ? json(nullptr) : json(score));
        }
        return json{{"scores", out}, {"canceled", cancelable && cancel()}};
    }

    // --- the switches the window sets ---------------------------------------
    //
    // Whose they are depends on who asks: an agent of a build heeds the
    // build's Stop and Wrap up, a chat turn's routing heeds the chat's Stop.
    if (method == "engine.flags") {
        return json{{"stop", building && cook_stop_.load(std::memory_order_relaxed)},
                    {"cancel", building ? cook_cancel_.load(std::memory_order_relaxed)
                                        : cancel_.load(std::memory_order_relaxed)},
                    {"running", running_.load(std::memory_order_relaxed)},
                    {"auto_edits", auto_edits_.load(std::memory_order_relaxed)}};
    }
    if (method == "mood") {
        // The status line is one line. While a chat turn is being answered it
        // is the turn's, and a build's agents report in their own pane.
        if (building && busy_.load(std::memory_order_relaxed)) {
            return json::object();
        }
        const std::string mood = params.value("mood", "idle");
        const Mood as = mood == "thinking" ? Mood::Thinking
                      : mood == "loading"  ? Mood::Loading
                      : mood == "routing"  ? Mood::Routing
                      : mood == "error"    ? Mood::Error
                                           : Mood::Idle;
        state_.set_mood(as, params.value("text", ""));
        if (params.contains("linked")) {
            const json& linked = params["linked"];
            state_.set_linked(linked.is_string() && !linked.get<std::string>().empty()
                                  ? std::optional<ExpertId>(linked.get<std::string>())
                                  : std::nullopt);
        }
        if (wake_) {
            wake_();
        }
        return json::object();
    }

    return serve_cook(method, params);
}

RouteDecision Engine::resolve(const Request& request, std::string& error) {
    // What was attached says something about who should answer: "what does
    // this do" means one thing beside main.rs and another beside a lease
    // agreement. The names are enough to say it.
    std::string routed = request.prompt;
    if (!request.pinned && !request.attachments.empty()) {
        routed += "\n\nAttached:";
        for (const attach::Attachment& one : request.attachments) {
            routed += " " + (one.name.empty() ? std::filesystem::path(one.path).filename().string()
                                              : one.name);
        }
    }

    RouteDecision decision;
    if (!wait_for_python(error)) {
        return decision;
    }
    nlohmann::json answer;
    try {
        nlohmann::json asked = routing_request(routed, request.pinned);
        asked["context"] = "chat";
        answer = orchestra_.call("route", asked);
    } catch (const std::exception& e) {
        error = std::string("routing failed: ") + e.what();
        return decision;
    }
    decision.expert     = answer.value("expert", "");
    decision.confidence = answer.value("confidence", 0.0F);
    decision.source     = route_source_from_name(answer.value("source", "fallback"));
    decision.detail     = answer.value("detail", "");

    const Config config = this->config();
    if (!request.pinned && !config.routing.keep_delegator_loaded
        && !config.expert(decision.expert).remote()) {
        // Its work for this prompt is done, and the expert is about to want
        // every byte it was holding. Unless the expert is somewhere else: then
        // nothing is about to be loaded, and freeing the delegator would only
        // mean loading it again for the next prompt.
        const std::lock_guard<std::mutex> host(host_mutex_);
        release_router();
    }
    return decision;
}

Engine::HeldModel Engine::hold_model(const ExpertId& id, const ModelParams& params,
                                     const std::string& name, const CancelCallback& cancel,
                                     long& load_ms, std::string& error) {
    load_ms = 0;
    HeldModel held;
    if (params.remote()) {
        // Nothing to swap and nothing to hold. Whatever is resident stays
        // resident: it costs nothing to leave, and the next prompt may well
        // be for it.
        held.model = hub_.model(params, error);
        return held;
    }

    bool told = false;   // whether the status line has said it is waiting
    for (;;) {
        bool                crowded = false;
        const std::uint64_t since   = leases_.releases();
        {
            const std::lock_guard<std::mutex> host(host_mutex_);
            ChatModel* model = mlx::is_model_dir(params.path)
                                   ? serve_mlx(id, params, name, cancel, load_ms, error, crowded)
                                   : load_gguf(id, params, name, cancel, load_ms, error, crowded);
            if (model != nullptr) {
                // Leased before the lock is let go, or another seat could
                // free it in between.
                held.model = model;
                held.lease = leases_.take(params.path);
                return held;
            }
        }
        if (!crowded) {
            return held;   // a real failure, or a stop, said in `error`
        }
        // The models other seats are using leave no room. Wait for one of
        // them to be let go, rather than pull it out from under its agent.
        if (!told) {
            told = true;
            state_.set_seat(id, SeatPhase::Loading, -1.0F);
            state_.set_mood(Mood::Loading, name + " is waiting for memory the build's agents are using");
            if (wake_) {
                wake_();
            }
        }
        if (!leases_.wait_for_release(cancel, since) && cancel && cancel()) {
            state_.set_seat(id, SeatPhase::Dormant);
            error = "stopped";
            return held;
        }
        error.clear();
    }
}

ChatModel* Engine::serve_mlx(const ExpertId& id, const ModelParams& params, const std::string& name,
                             const CancelCallback& cancel, long& load_ms, std::string& error,
                             bool& crowded) {
    // A folder rather than a file: an MLX model, which llama.cpp cannot read
    // and MLX's own server can. See mlx_server.hpp.
    if (!mlx_.serving(params.path)) {
        // One MLX server at a time: the one there may go only when nobody is
        // using its model.
        if (mlx_.running() && leases_.held(mlx_.served().string())) {
            crowded = true;
            return nullptr;
        }
        // The GGUF experts nobody is using go too, as one model in memory
        // has always meant. One that is in use stays when the machine has
        // the room for both.
        host_->release_experts(in_use());
        if (host_->expert_count() > 0 && !host_->fits_beside(params)) {
            crowded = true;
            return nullptr;
        }
        stop_mlx();
        state_.set_resident(host_->loaded_expert());
        show_loaded();
        // A spinner rather than a figure: MLX does not say how far along it
        // is, and a percentage that is not measuring anything is worse than
        // none.
        state_.set_seat(id, SeatPhase::Loading, -1.0F);
        state_.set_mood(Mood::Loading, "starting " + name + " in MLX");
        if (wake_) {
            wake_();
        }
        const auto started = Clock::now();
        if (!mlx_.serve(params.path, cancel, error)) {
            state_.set_seat(id, SeatPhase::Dormant);
            return nullptr;
        }
        load_ms = ms_since(started);
    }
    mlx_seat_ = id;
    state_.set_resident(id);
    show_loaded();
    state_.set_seat(id, SeatPhase::Dormant);
    if (wake_) {
        wake_();
    }
    return hub_.local_server(mlx_.base_url(), params.n_ctx);
}

ChatModel* Engine::load_gguf(const ExpertId& id, const ModelParams& params, const std::string& name,
                             const CancelCallback& cancel, long& load_ms, std::string& error,
                             bool& crowded) {
    // A GGUF is about to take the memory an MLX model is holding -- unless
    // somebody is using that model, when both stay if they fit.
    if (mlx_.running() && !leases_.held(mlx_.served().string())) {
        stop_mlx();
    }

    const bool already_resident = host_->loaded_expert() == id;
    if (!already_resident) {
        state_.set_seat(id, SeatPhase::Loading, 0.0F);
        state_.set_mood(Mood::Loading, "swapping in " + name);
        if (wake_) {
            wake_();
        }
    }

    // acquire_expert frees whoever nobody is using before loading the next,
    // which is the whole memory argument for the design: with one seat at a
    // time the peak is the larger of the two experts, never their sum.
    const auto load_start = Clock::now();
    LoadedModel* model = host_->acquire_expert(
        id, params,
        [this, &id](float progress) {
            state_.set_seat_progress(id, progress);
            if (wake_) {
                wake_();
            }
        },
        cancel, error, in_use(), &crowded);
    if (model == nullptr) {
        state_.set_seat(id, SeatPhase::Dormant);
        state_.set_resident(host_->loaded_expert());
        show_loaded();
        return nullptr;
    }
    load_ms = already_resident ? 0 : ms_since(load_start);
    state_.set_resident(id);
    show_loaded();
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

    const CancelCallback cancel = [this] {
        return cancel_.load(std::memory_order_relaxed) || !running_.load(std::memory_order_relaxed);
    };

    // The settings as they are when the turn starts, kept for the whole of
    // it: a build running beside it can add a seat to the engine's own copy
    // at any moment, and a turn read halfway through a change is a turn
    // answered by half of two configurations.
    const Config config = this->config();

    // --- route -------------------------------------------------------------
    state_.set_mood(Mood::Routing, "Crucible is reading the prompt");
    if (wake_) {
        wake_();
    }

    std::string routing_error;
    const RouteDecision decision = resolve(request, routing_error);
    if (!routing_error.empty()) {
        if (routing_error == "stopped") {
            state_.cancel_turn(turn);
            state_.set_mood(Mood::Idle);
        } else {
            state_.fail_turn(turn, routing_error);
            state_.set_mood(Mood::Error, routing_error);
        }
        return;
    }
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

    if (!config.has_expert(decision.expert)) {
        state_.fail_turn(turn,
            "No expert has a model. Add one in Settings, Experts.");
        state_.set_linked(std::nullopt);
        state_.set_mood(Mood::Error, "no experts configured");
        return;
    }

    // --- JIT swap ----------------------------------------------------------
    const ModelParams& params = config.expert(decision.expert);

    // The display name, resolved once. Every status line below wants it, and a
    // seat ejected mid-turn would otherwise make each of them fall back to the
    // raw id independently.
    const std::string expert_name = expert_label(config.roster, decision.expert);

    long        load_ms = 0;
    std::string error;
    // Held for the whole turn: the lease is what keeps a build's thread from
    // freeing this model while the turn is answering with it.
    HeldModel   held   = hold_model(decision.expert, params, expert_name, cancel, load_ms, error);
    ChatModel*  expert = held.model;

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
    messages.push_back({"system", config.system_prompt});
    if (!config.reasoning_effort.empty() && !params.remote() && expert->takes_effort()) {
        // Where a local reasoning model looks for it. See
        // Config::reasoning_effort. A provider takes it as a field of the
        // request instead, which is what ChatRequest::effort is for.
        messages.front().content += "\n\nReasoning: " + config.reasoning_effort;
    }
    if (config.tools.web_search) {
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
        const Overflow policy = overflow_from_id(config.tools.overflow);
        const int used   = expert->prompt_tokens(messages);
        const int budget = static_cast<int>(
            static_cast<double>(expert->context_size()) * kPromptShare);

        if (policy == Overflow::StopAtLimit && used > budget) {
            // Refused rather than shortened. The model cannot tell you what it
            // stopped being able to see, so the program says it instead.
            state_.set_linked(std::nullopt);
            state_.fail_turn(
                turn, "this conversation no longer fits: " + std::to_string(used) + " of "
                      + std::to_string(expert->context_size())
                      + " tokens. Raise \"Context\" in Settings, Generation, or start a new chat.");
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
    if (config.tools.web_search) {
        rounds = std::max(rounds, config.tools.search_rounds + 1);
    }
    if (workshop.enabled) {
        rounds = std::max(rounds, kToolRounds);
    }
    std::vector<std::string> already_searched;
    std::vector<std::string> already_done;   // project calls made this turn

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
            expert->chat(ChatRequest{messages, params, config.reasoning_effort}, sink);
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

            // The turn so far, and this round's prose up to the call.
            const std::string kept = paragraphs(earlier, prose_before_tool_call(answer, call->kind));

            // The same call twice in one turn is a model going round, and the
            // second answer would be the first one again. Not run, and not
            // drawn: it is told its result is already above. A write or an
            // edit is the exception -- changing a file again is how a fix to
            // it is made.
            const bool changes_a_file =
                call->kind == tools::ToolKind::Write || call->kind == tools::ToolKind::Edit;
            const std::string asked_for = std::string(tools::tool_kind_name(call->kind)) + '\n'
                                        + call->argument;
            if (!changes_a_file
                && std::find(already_done.begin(), already_done.end(), asked_for)
                       != already_done.end()) {
                earlier = kept;
                state_.set_reply(turn, earlier);
                messages.push_back({"assistant", answer});
                messages.push_back({"user", "You already did exactly that this turn, and its "
                                            "result is above. Use it -- and if it told you what "
                                            "you needed, answer now."});
                continue;
            }
            already_done.push_back(asked_for);

            // --- the gate ---------------------------------------------
            //
            // A write or an edit is the tool call that changes something the
            // user owns, so it is the one that stops and asks -- unless they
            // have said not to. Everything else (reading, listing, running)
            // either changes nothing or was already agreed to by trusting the
            // folder. An edit is asked about as the file it would leave: an
            // edit that would not apply is not asked about at all, and fails
            // with its reason when it is run.
            std::optional<tools::ToolCall> to_approve;
            if (changes_a_file && !auto_edits_.load()) {
                to_approve = *call;
                if (call->kind == tools::ToolKind::Edit) {
                    std::string why;
                    const std::optional<std::string> after = tools::edited_contents(*call, workshop, why);
                    to_approve = after ? std::optional<tools::ToolCall>(tools::ToolCall{
                                             tools::ToolKind::Write, call->argument, *after, {}})
                                       : std::nullopt;
                }
            }
            if (to_approve && !await_edit_approval(turn, kept, *to_approve, workshop)) {
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

            tools::SearchSettings searching;
            searching.enabled         = config.tools.web_search;
            searching.provider        = config.tools.search_provider;
            searching.endpoint        = config.tools.search_endpoint;
            searching.api_key         = config.tools.search_api_key;
            searching.max_results     = config.tools.search_results;
            searching.timeout_seconds = config.tools.search_timeout;
            const tools::ToolResult result = tools::run_tool(
                *call, workshop, searching,
                [this] { return cancel_.load(std::memory_order_relaxed); });
            // The diff a write made, or the output a command printed, kept for
            // the transcript rather than only handed to the model.
            state_.add_action(turn, TurnAction{result.summary, result.detail,
                                               changes_a_file ? call->argument : std::string(),
                                               result.picture_path});

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
            // A picture the tool produced goes to a model that can see one,
            // as a picture; one that cannot is told so, beside whatever words
            // were read off it.
            ChatMessage back{"user", {}};
            if (!result.pictures.empty()) {
                if (expert->sees_images()) {
                    for (const attach::Image& image : result.pictures) {
                        back.images.push_back({image.mime, image.data});
                    }
                } else {
                    handback += "\n(This model reads text only and cannot see the picture itself.)";
                }
            }
            if (round + 2 >= rounds) {
                handback += "\n\nThat was the last action available this turn. Write "
                            "the answer now from what you have.";
            }
            back.content = std::move(handback);
            messages.push_back(std::move(back));
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
        settings.enabled         = config.tools.web_search;
        settings.provider        = config.tools.search_provider;
        settings.endpoint        = config.tools.search_endpoint;
        settings.api_key         = config.tools.search_api_key;
        settings.max_results     = config.tools.search_results;
        settings.timeout_seconds = config.tools.search_timeout;

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
                    why = "ran out of turns before finishing -- ask again to carry on";
                } else if (asked_to_search) {
                    why = "kept searching instead of answering -- raise \"Searches per "
                          "prompt\" in Settings, Tools";
                } else if (stats.hit_limit) {
                    why = "thought until \"Longest reply\" ran out -- raise it in Settings, "
                          "Generation, or lower the effort";
                } else {
                    why = "stopped without answering";
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
