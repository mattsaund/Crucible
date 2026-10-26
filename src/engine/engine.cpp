// SPDX-License-Identifier: MIT
//
// The delegation loop.
//
// The chat's turns are taken one at a time by the worker thread, which opens
// each in the transcript and hands it to the orchestrator: its rounds are
// decided there and served here (see engine_chat.cpp). The UI hands over a
// prompt and learns how it went through AppState plus a wake callback -- it
// never calls into the engine's internals, and the engine never calls into
// the window toolkit.
//
// The worker is not the only thread that reaches llama.cpp: a build's agents,
// the orchestrator's requests and a load for the box's choice each run on
// threads of their own. Every load and free takes host_mutex_, and a model
// somebody is generating with is held by a lease, so none of them frees it
// under its user. See leases.hpp.
//
// Config changes and expert releases ride the same queue as the chat's
// prompts, so they are applied in order and never land in the middle of a
// chat turn.
#include "crucible/engine/engine.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>

#include "crucible/config/gpu_policy.hpp"
#include "crucible/config/paths.hpp"
#include "crucible/lab/python.hpp"
#include "crucible/llm/response_filter.hpp"
#include "crucible/tools/workshop.hpp"

#include <fstream>
#include <sstream>
#include "crucible/runtime/registry.hpp"
#include "crucible/session/store.hpp"
#include "crucible/util/format.hpp"

namespace crucible {
namespace {

using Clock = std::chrono::steady_clock;

long ms_since(Clock::time_point start) {
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

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
    // and starting a server twice would be two of it. One going already is
    // told to go round again rather than waited for: a server fetched by npx
    // takes its minute, and the engine's queue is not held for it.
    if (!mcp_.any()) {
        return;
    }
    warm_again_ = true;
    if (mcp_warming_) {
        return;
    }
    if (mcp_thread_.joinable()) {
        mcp_thread_.join();   // over: it said so before it ended
    }
    mcp_warming_ = true;
    mcp_thread_  = std::thread([this] {
        for (;;) {
            {
                const std::lock_guard<std::mutex> again(mcp_mutex_);
                if (!warm_again_ || !running_.load(std::memory_order_relaxed)) {
                    mcp_warming_ = false;
                    return;
                }
                warm_again_ = false;
            }
            mcp_.warm();
        }
    });
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
    // So does an MCP server still starting, for as long as a minute and a
    // half, and a call to one: the warm-up, the worker and a build can each
    // be in either. They end now, before anything is joined, and no server
    // starts after.
    mcp_.stop_all();
    queued_.notify_all();
    {
        const std::lock_guard<std::mutex> lock(prepare_mutex_);   // so the wait cannot miss it
    }
    prepare_wanted_.notify_all();
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
    // A benchmark run the same way: told to stop, given a moment, and the
    // orchestrator stopped under it if it does not.
    bench_stop_.store(true, std::memory_order_relaxed);
    if (bench_thread_.joinable()) {
        for (int waited = 0; waited < 50 && benching_.load(std::memory_order_relaxed); ++waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (benching_.load(std::memory_order_relaxed)) {
            orchestra_.stop();
        }
        bench_thread_.join();
    }
    // And a chat turn, whose loop is the orchestrator's too. Every wait its
    // calls can be in has just been told to stop, so one still being answered
    // a few seconds on is waiting on an orchestrator that has hung -- and
    // stopping that ends the call the worker is blocked in.
    for (int waited = 0; waited < 50 && busy_.load(std::memory_order_relaxed); ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (busy_.load(std::memory_order_relaxed)) {
        orchestra_.stop();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    // Nothing is left running behind a window that is gone: not what experts
    // started, and not the MCP servers, which were stopped above.
    processes_.stop_all();
    // Taken out under the lock and joined outside it: the warm-up asks for
    // the lock to see whether to go round again.
    std::thread warming;
    {
        const std::lock_guard<std::mutex> lock(mcp_mutex_);
        warm_again_ = false;
        warming     = std::move(mcp_thread_);
    }
    if (warming.joinable()) {
        warming.join();
    }
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

void Engine::name_session(std::string session, std::filesystem::path root, std::string excerpt,
                          bool spare) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        Request request;
        request.kind    = RequestKind::NameSession;
        request.session = std::move(session);
        request.root    = std::move(root);
        request.prompt  = std::move(excerpt);
        request.spare   = spare;
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
    // A turn parked on an edit is inside ask_about_edit, not looking at the
    // flag. Waking it is what lets Stop end a turn that is waiting on you.
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
    // Queued rather than done inline, like a config change: it lands between
    // the chat's prompts, in the order it was asked for, never in the middle
    // of a turn.
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        Request request;
        request.kind = RequestKind::ReleaseExpert;
        pending_.push_back(std::move(request));
    }
    queued_.notify_one();
}

void Engine::choose(std::optional<ExpertId> seat, bool load) {
    bool changed = false;
    {
        const std::lock_guard<std::mutex> lock(chosen_mutex_);
        changed = chosen_ != seat;
        chosen_ = seat;
    }
    // A load under way for an earlier choice stops: this one is wanted now.
    if (changed) {
        choices_.fetch_add(1);
    }
    if (!load) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(prepare_mutex_);
        if (!running_.load(std::memory_order_relaxed)) {
            return;
        }
        prepare_seat_ = std::move(seat);
        ++prepare_asked_;
        if (!prepare_thread_.joinable()) {
            prepare_thread_ = std::thread(&Engine::prepare_loop, this);
        }
    }
    prepare_wanted_.notify_one();
}

void Engine::prepare_loop() {
    std::uint64_t done = 0;
    for (;;) {
        std::optional<ExpertId> seat;
        std::uint64_t           choice = 0;
        {
            std::unique_lock<std::mutex> lock(prepare_mutex_);
            prepare_wanted_.wait(lock, [this, &done] {
                return prepare_asked_ != done || !running_.load(std::memory_order_relaxed);
            });
            if (!running_.load(std::memory_order_relaxed)) {
                return;
            }
            seat   = prepare_seat_;
            done   = prepare_asked_;
            choice = choices_.load();
        }
        // A load that throws -- a corrupt GGUF -- is said, not fatal.
        try {
            do_prepare(seat, choice);
        } catch (const std::exception& e) {
            state_.set_mood(Mood::Error, e.what());
        } catch (...) {
            state_.set_mood(Mood::Error, "the model did not load");
        }
        if (wake_) {
            wake_();
        }
    }
}

std::string Engine::chosen_path() const {
    std::optional<ExpertId> seat;
    {
        const std::lock_guard<std::mutex> lock(chosen_mutex_);
        seat = chosen_;
    }
    if (!seat) {
        return {};
    }
    const Config       config = this->config();
    const ModelParams& params = config.expert(*seat);
    return params.model.empty() || params.remote() ? std::string() : params.path;
}

bool Engine::seat_chosen() const {
    const std::lock_guard<std::mutex> lock(chosen_mutex_);
    return chosen_.has_value();
}

void Engine::do_prepare(const std::optional<ExpertId>& seat, std::uint64_t choice) {
    // Nothing loads for a turn while a benchmark runs, and no turn is
    // answered then either: the choice is kept, and loads with the first
    // prompt after the run.
    if (benching()) {
        return;
    }
    // Back to the delegator: the table as any prompt leaves it.
    if (!seat) {
        tidy_models();
        return;
    }
    const Config      config = this->config();
    const ModelParams params = config.expert(*seat);
    if (params.model.empty() || params.remote()) {
        return;   // a provider's model is a request away, not a load
    }
    if (!config.routing.keep_delegator_loaded) {
        release_router_between_routes();
    }
    const CancelCallback cancel = [this, choice] {
        return choices_.load(std::memory_order_relaxed) != choice || !running_.load(std::memory_order_relaxed);
    };
    long        load_ms = 0;
    std::string error;
    const std::string name = expert_label(config.roster, *seat);
    // Loaded and let go at once: the lease is for a turn, and this is not one.
    const HeldModel held = hold_model(*seat, params, name, cancel, load_ms, error);
    if (held.model == nullptr && !error.empty() && error != "stopped") {
        state_.set_mood(Mood::Error, name + " did not load: " + error);
    } else if (!is_busy() && !cooking()) {
        state_.set_mood(Mood::Idle);
    }
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
        // Only while a question is up. A second click on Allow, or one after
        // Stop, was kept, and answered the next edit before it was shown.
        if (!edit_asking_) {
            return;
        }
        edit_approved_ = approved;
    }
    edit_answered_.notify_all();
}

bool Engine::ask_about_edit(const tools::ToolCall& call,
                            const tools::WorkshopSettings& workshop,
                            const CancelCallback& cancel) {
    // What the file is now, so the two can be shown side by side. A path that
    // does not resolve inside the root is not a question to put to the user --
    // run_tool would refuse it anyway, and asking would be asking them to
    // approve something that cannot happen. Nor is a sentence where the path
    // goes: a 1.2B model's WRITE line was its account of the change, and the
    // person was asked about a file named after it.
    const std::optional<std::filesystem::path> target =
        tools::plausible_path(call.argument) ? tools::resolve_in_root(workshop.root, call.argument)
                                             : std::nullopt;
    if (!target) {
        return true;   // let run_tool produce the refusal and its explanation
    }

    // One edit on screen at a time: an agent of the build and the chat may
    // both want a yes, and the window asks about one file at once. Waited
    // for in a way a Stop ends, since the one on screen may be a build's
    // question nobody answers for an hour.
    std::unique_lock<std::mutex> one_at_a_time(asking_mutex_, std::defer_lock);
    while (!one_at_a_time.try_lock()) {
        if ((cancel && cancel()) || !running_.load(std::memory_order_relaxed)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
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

    {
        // Open before it is shown, so the answer to it is the first one kept.
        const std::lock_guard<std::mutex> lock(edit_mutex_);
        edit_approved_.reset();
        edit_asking_ = true;
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
        edit_asking_ = false;
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
                              ? "the runtime found no hardware (see Settings, Runtimes)"
                              : "no runtime: install one in Settings, Runtimes");
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
            state_.set_mood(Mood::Idle, kept ? "released what is not in use (the build is using the rest)"
                                      : all ? "nothing loaded" : "expert released");
            if (wake_) {
                wake_();
            }
            continue;
        }

        if (request.kind == RequestKind::ApplyConfig) {
            do_apply_config(std::move(request.config));
            // Outside the models' lock, which loads and prompts wait on.
            warm_mcp(config());
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

        // Sent before a benchmark began and reached only once it had, behind
        // something else in the queue: refused as one sent during it is, and
        // before settle(), which would bring the delegator back beside it.
        if (benching()) {
            std::vector<TurnAttachment> tiles;
            for (const attach::Attachment& one : request.attachments) {
                tiles.push_back(attach::tile_of(one));
            }
            state_.fail_turn(state_.begin_turn(request.prompt, std::move(tiles)), "benchmark running");
            if (wake_) {
                wake_();
            }
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

    // A load for the box's choice has been told to stop; it is gone before
    // the models are.
    std::thread preparing;
    {
        const std::lock_guard<std::mutex> lock(prepare_mutex_);
        preparing = std::move(prepare_thread_);
    }
    prepare_wanted_.notify_all();
    if (preparing.joinable()) {
        preparing.join();
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

void Engine::load_router(const CancelCallback& cancel) {
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
        [this, &cancel] {
            return (cancel ? cancel() : cancel_.load(std::memory_order_relaxed))
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

void Engine::ensure_router(const CancelCallback& cancel) {
    if (config().router.model.empty() || host_->router() != nullptr) {
        return;  // keywords, which need nothing loaded, or already there
    }
    load_router(cancel);
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
    const Snapshot    before = state_.snapshot();
    const bool        demand = !config().routing.keep_delegator_loaded;
    const std::string chosen = chosen_path();
    {
        const std::lock_guard<std::mutex> host(host_mutex_);
        // On demand: the experts nobody is using go, so the delegator has
        // the room. One a build's agent holds stays -- it is mid-task -- and
        // so does the one chosen in the box, which the next prompt is for.
        if (demand) {
            const std::optional<ExpertId> was = host_->loaded_expert();
            host_->release_experts([this, &chosen](const std::string& path) {
                return leases_.held(path) || (!chosen.empty() && path == chosen);
            });
            if (was && host_->loaded_expert() != was) {
                state_.set_seat(*was, SeatPhase::Dormant);
            }
            state_.set_resident(host_->loaded_expert());
            show_loaded();
            if (!leases_.held(mlx_.served().string()) && mlx_.served().string() != chosen) {
                stop_mlx();
            }
        }
        // With an expert chosen nothing is routed, and on demand the
        // delegator would only take the chosen one's room, or with a
        // provider's chosen, be gigabytes loaded for nothing.
        if (!demand || !seat_chosen()) {
            ready_delegator();
        }
    }
    // Whatever the last request ended by saying -- "canceled", an error -- is
    // still the thing worth reading once the delegator is back.
    state_.set_mood(before.mood == Mood::Error ? Mood::Error : Mood::Idle, before.status);
    if (wake_) {
        wake_();
    }
}

void Engine::release_router_between_routes() {
    // A route is a second or two; one that has said nothing for a minute is
    // one that will not, and is not waited for.
    const auto stale = [this] {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch()).count();
        return now - routes_touched_ms_.load() > 60'000;
    };
    {
        std::unique_lock<std::mutex> lock(routes_mutex_);
        routes_done_.wait_for(lock, std::chrono::seconds(10),
                              [this, &stale] { return routes_open_.load() == 0 || stale(); });
    }
    const std::lock_guard<std::mutex> host(host_mutex_);
    if (stale()) {
        routes_open_.store(0);
    }
    if (routes_open_.load() == 0) {
        release_router();
    }
    // Otherwise a route is still using it: it stays, and the next prompt or
    // seat that wants the room frees it.
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
    state_.set_mood(Mood::Idle, freed ? "settings applied: models load with them from the next prompt"
                                      : "settings applied");
}

void Engine::do_name_session(const Request& request) {
    // The orchestrator's: it asks the delegator, when there is one, and takes
    // the opening words of the first prompt when there is not. Without the
    // orchestrator there is no name, and the list shows that first prompt,
    // which is what it showed before there were names.
    std::string name;
    std::string error;
    if (wait_for_python(error)) {
        nlohmann::json asked{{"excerpt", request.prompt}, {"context", "chat"}};
        if (request.spare) {
            asked["spare"] = true;
        }
        try {
            name = orchestra_.call("name.session", asked).value("name", "");
        } catch (const std::exception&) {
            name.clear();
        }
    }
    // Said when there is none as well: whoever names old conversations one
    // at a time is waiting on it to ask for the next.
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

bool Engine::wait_for_python(std::string& error, const CancelCallback& cancel) {
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
            if ((cancel ? cancel() : cancel_.load(std::memory_order_relaxed))
                || !running_.load(std::memory_order_relaxed)) {
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
            "downloaded when Crucible starts: check the connection and restart Crucible, "
            "or run crucible --install-python.";
    return false;
}

nlohmann::json Engine::routing_request(const Config& config, const std::string& prompt,
                                       const std::optional<ExpertId>& pinned) {
    using json = nlohmann::json;
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
    if (params.value("context", std::string()) == "bench") {
        return [this] {
            return bench_stop_.load(std::memory_order_relaxed) || !running_.load(std::memory_order_relaxed);
        };
    }
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
    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    // A switch the orchestrator set, or `otherwise` when it set none.
    const auto said = [&params](const char* key, bool otherwise) {
        const auto found = params.find(key);
        return found != params.end() && found->is_boolean() ? found->get<bool>() : otherwise;
    };
    if (method == "delegator.ready") {
        // Something that can do without it -- a conversation's name -- does
        // not load it while an expert is chosen in the box, when it is loaded
        // on demand: over a local expert it would take that expert's room
        // (see choose()), and beside a provider's it is gigabytes read for
        // three words. The name waits for a prompt that loads it anyway. And
        // what says "load": false -- an old conversation named in passing --
        // uses it only if it is here.
        const bool spare = (said("optional", false) && !config().routing.keep_delegator_loaded && seat_chosen())
                        || !said("load", true);
        const std::lock_guard<std::mutex> host(host_mutex_);
        if (!spare) {
            ensure_router(cancel);
        }
        const bool loaded = host_->router() != nullptr;
        // A route has begun: see routes_open_. Closed by delegator.done.
        routes_open_.fetch_add(1);
        routes_touched_ms_.store(now_ms());
        return json{{"available", loaded}, {"path", loaded ? config().router.path : std::string()}};
    }
    if (method == "delegator.done") {
        {
            const std::lock_guard<std::mutex> host(host_mutex_);
            if (routes_open_.load() > 0) {
                routes_open_.fetch_sub(1);
            }
        }
        { const std::lock_guard<std::mutex> lock(routes_mutex_); }
        routes_done_.notify_all();
        return json::object();
    }
    if (method.rfind("delegator.", 0) == 0) {
        routes_touched_ms_.store(now_ms());
    }
    if (method == "delegator.generate") {
        // A few words of writing -- a conversation's name, an expert's
        // examples. A reasoning model thinks first: its thinking is filtered
        // out, it is given room for it, and asked for as little of it as it
        // takes.
        const std::lock_guard<std::mutex> host(host_mutex_);
        if (said("load", true)) {
            ensure_router(cancel);   // freed since delegator.ready by somebody who could not wait
        }
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
        ensure_router(cancel);   // freed since delegator.ready by somebody who could not wait
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
            model->score_labels(params.value("prompt", ""), labels, cancelable ? cancel : CancelCallback{},
                                params.value("prefix", ""));
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

    if (method.rfind("bench.", 0) == 0) {
        return serve_bench(method, params);
    }
    if (method.rfind("turn.", 0) == 0) {
        return serve_turn(method, params);
    }
    return serve_cook(method, params);
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
    // A GGUF is about to take the memory an MLX model is holding, unless
    // somebody is using that model. Then both stay if they fit; and if they
    // do not, the GGUF waits for it to be let go, as it would for an expert
    // in use. acquire_expert asks, once it has freed what nobody is using.
    if (mlx_.running() && !leases_.held(mlx_.served().string())) {
        stop_mlx();
    }
    const bool beside_mlx = mlx_.running();

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
        cancel, error, in_use(), &crowded, beside_mlx);
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

}  // namespace crucible
