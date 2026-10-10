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
#include <thread>
#include <utility>

#include <fstream>
#include <iterator>

#include "crucible/config/paths.hpp"
#include "crucible/lab/python.hpp"
#include "crucible/llm/response_filter.hpp"
#include "crucible/tools/git.hpp"
#include "crucible/tools/workshop.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/text.hpp"

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

tools::ToolCall call_from(const json& params) {
    tools::ToolCall call;
    call.kind     = tool_kind_from_name(params.value("kind", ""));
    call.argument = params.value("argument", "");
    call.content  = params.value("content", "");
    call.shell    = params.value("shell", "");
    return call;
}

/// A seat's parameters for a model named the way a seat names one: a file
/// in the models folder, or a provider's model.
ModelParams params_for(const Config& config, const std::string& model, const std::string& provider) {
    ModelParams params = config.defaults;
    params.model       = model;
    params.provider    = model.empty() ? std::string() : provider;
    if (params.remote()) {
        // The cap is sized for a card; a provider counts its thinking against
        // it. The same rule the settings screen applies when a seat is pointed
        // at a provider.
        params.max_tokens = -1;
    }
    return params;
}

}  // namespace

// ---------------------------------------------------------------------------
// Queueing and control
// ---------------------------------------------------------------------------

void Engine::set_project(std::filesystem::path root, std::filesystem::path project_dir) {
    project_root_ = std::move(root);
    project_dir_  = project_dir;
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
    // The rest of what an expert can reach, each behind its own switch.
    workshop.web              = config_.tools.web_search;
    workshop.computer_control = config_.tools.computer_control;
    workshop.python           = lab::python::installed() ? lab::python::interpreter()
                                                         : std::filesystem::path();
    workshop.scratch          = paths::scratch_dir();
    workshop.processes        = const_cast<tools::Processes*>(&processes_);
    workshop.mcp              = const_cast<tools::mcp::Hub*>(&mcp_);
    return workshop;
}

std::string Engine::start_cook(std::string goal, int budget_seconds, std::filesystem::path root,
                               std::vector<attach::Attachment> attachments,
                               std::optional<ExpertId> pinned, std::string kind) {
    if (cooking_.exchange(true)) {
        return "a build is already running -- stop it, or wait for it to finish";
    }
    // The thread of the build before this one has finished -- cooking_ said
    // so -- and is joined here rather than when it ended, because a thread
    // cannot join itself.
    if (cook_thread_.joinable()) {
        cook_thread_.join();
    }
    cook_stop_.store(false, std::memory_order_relaxed);
    cook_cancel_.store(false, std::memory_order_relaxed);
    cook_thread_ = std::thread([this, goal = std::move(goal), budget_seconds, root = std::move(root),
                                attachments = std::move(attachments), pinned = std::move(pinned),
                                kind = std::move(kind)]() mutable {
        // The worker makes the model host as it starts, and a build started
        // in the same breath as the engine -- a script, the smoke test -- must
        // not ask for a model before there is anything to ask.
        while (!host_ready_.load(std::memory_order_acquire) && running_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        // Contained like a chat turn is. A build is an hour of running
        // whatever a model asks for, so an exception escaping here would take
        // the process, and the window with it, down mid-edit.
        try {
            do_cook(goal, budget_seconds, root, std::move(attachments), std::move(pinned), kind);
        } catch (const std::exception& e) {
            state_.set_mood(Mood::Error, e.what());
            state_.add_notice(std::string("build failed: ") + e.what());
        } catch (...) {
            state_.set_mood(Mood::Error, "build failed");
        }
        {
            // Whatever seats the orchestrator did not hand back go now, with
            // the leases that hold their models.
            const std::lock_guard<std::mutex> lock(seats_mutex_);
            seats_.clear();
        }
        cooking_.store(false, std::memory_order_relaxed);
        if (wake_) {
            wake_();
        }
        tidy_models();
    });
    return {};
}

void Engine::stop_cook() {
    cook_stop_.store(true, std::memory_order_relaxed);
    // A cook parked on a question is inside await_cook_answer, not looking at
    // the flag. Waking it is what lets /stop end a cook that is waiting on you.
    cook_answered_.notify_all();
}

void Engine::cancel_cook() {
    cook_stop_.store(true, std::memory_order_relaxed);
    cook_cancel_.store(true, std::memory_order_relaxed);
    // Every place an agent can be waiting: on the person's answer, on an
    // edit's yes, on a model another seat holds, on a provider's next line.
    // Missing one of these was a lockup before builds had threads of their
    // own -- Stop now on a cook that was asking something left the worker
    // waiting for an answer that would never come.
    cook_answered_.notify_all();
    edit_answered_.notify_all();
    leases_.wake_all();
    hub_.interrupt();
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
            || cook_cancel_.load(std::memory_order_relaxed)
            || !running_.load(std::memory_order_relaxed);
    });
    return std::exchange(cook_answer_, std::nullopt);
}

void Engine::publish_cook() {
    // With cook_mutex_ held, by every caller: the journal is replaced by
    // whichever agent's thread reported last.
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
        return cook_cancel_.load(std::memory_order_relaxed)
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

std::shared_ptr<Engine::AgentSeat> Engine::seat_named(const nlohmann::json& params) {
    const std::string handle = params.value("seat", std::string());
    const std::lock_guard<std::mutex> lock(seats_mutex_);
    const auto found = seats_.find(handle);
    return found == seats_.end() ? nullptr : found->second;
}

nlohmann::json Engine::serve_cook(const std::string& method, const nlohmann::json& params) {
    const CancelCallback cancel = cancel_for(params);

    // --- the seats -------------------------------------------------------------
    //
    // One per agent at work. Taking a seat holds its model -- a lease, for a
    // model on this machine -- until the seat is released, so an agent
    // halfway through a task keeps its model whatever the others load.
    if (method == "seat.take") {
        const Config      config = this->config();
        const ExpertId    id     = params.value("expert", "");
        const std::string name   = params.value("name", expert_label(config.roster, id));
        if (!config.has_expert(id)) {
            return json{{"ok", false}, {"error", name + " has no model"}};
        }
        const ModelParams seat_params = config.expert(id);
        // The delegator has done its routing, and an expert on this machine is
        // about to want every byte it was holding. See resolve().
        if (!config.routing.keep_delegator_loaded && !seat_params.remote()) {
            const std::lock_guard<std::mutex> host(host_mutex_);
            release_router();
        }
        state_.set_linked(id);
        if (wake_) {
            wake_();
        }
        auto seat    = std::make_shared<AgentSeat>();
        seat->id     = id;
        seat->name   = name;
        seat->params = seat_params;
        long        load_ms = 0;
        std::string error;
        seat->held = hold_model(id, seat_params, name, cancel, load_ms, error);
        if (seat->held.model == nullptr) {
            return json{{"ok", false}, {"error", error.empty() ? name + " could not be loaded" : error}};
        }
        std::string handle;
        {
            const std::lock_guard<std::mutex> lock(seats_mutex_);
            handle = "s" + std::to_string(++next_seat_);
            seats_[handle] = std::move(seat);
        }
        return json{{"ok", true}, {"seat", handle}, {"load_ms", load_ms},
                    {"remote", seat_params.remote()}};
    }
    if (method == "seat.release") {
        // The lease goes with the seat; a model nobody else holds may now be
        // freed for the next one, and anyone waiting for the room is woken.
        std::shared_ptr<AgentSeat> released;
        {
            const std::lock_guard<std::mutex> lock(seats_mutex_);
            const auto found = seats_.find(params.value("seat", std::string()));
            if (found != seats_.end()) {
                released = std::move(found->second);
                seats_.erase(found);
            }
        }
        return json::object();
    }
    if (method == "seat.chat") {
        const std::shared_ptr<AgentSeat> seat = seat_named(params);
        if (!seat) {
            return json{{"error", "no expert is in that seat"}, {"ms", 0}};
        }
        const std::lock_guard<std::mutex> one_round(seat->mutex);
        std::vector<ChatMessage> messages;
        for (const json& one : params.value("messages", json::array())) {
            ChatMessage message{one.value("role", "user"), one.value("content", "")};
            if (one.value("attached", false)) {
                message.images = seat->images;
            }
            // The pictures the last tool produced, for the round after it
            // and to a model that sees them. See AgentSeat::tool_images.
            if (one.value("tool_pictures", false) && !seat->tool_images.empty()
                && seat->held.model->sees_images()) {
                message.images = seat->tool_images;
            }
            messages.push_back(std::move(message));
        }
        const CookRound round = cook_round(*seat->held.model, seat->params, messages);
        seat->tool_images.clear();
        return json{{"answer", round.answer}, {"reasoning", round.reasoning},
                    {"error", round.error}, {"ms", round.ms}};
    }
    if (method == "seat.attachments") {
        const std::shared_ptr<AgentSeat> seat = seat_named(params);
        if (!seat || cook_attachments_.empty()) {
            return json{{"text", ""}, {"images", 0}};
        }
        const std::lock_guard<std::mutex> one_round(seat->mutex);
        // Read for this seat: its context is the size the share is of, and
        // whether it sees pictures is its own.
        attach::Composed composed =
            read_attachments(cook_attachments_, *seat->held.model,
                             {{"system", params.value("system", "")}}, params.value("share", 0.33));
        seat->images.clear();
        for (attach::Image& image : composed.images) {
            seat->images.push_back({std::move(image.mime), std::move(image.data)});
        }
        return json{{"text", composed.text}, {"images", seat->images.size()}};
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
        const tools::ToolResult result = tools::run_tool(call, cook_workshop_, cook_search_, cancel);
        // The pictures go with the next round of the seat whose tool made
        // them, when its model can see one.
        const std::shared_ptr<AgentSeat> seat = seat_named(params);
        std::size_t pictures = 0;
        std::string output   = result.output;
        if (!result.pictures.empty()) {
            if (seat && seat->held.model->sees_images()) {
                const std::lock_guard<std::mutex> one_round(seat->mutex);
                seat->tool_images.clear();
                for (const attach::Image& image : result.pictures) {
                    seat->tool_images.push_back({image.mime, image.data});
                }
                pictures = seat->tool_images.size();
            } else {
                output += "\n(This model reads text only and cannot see the picture itself.)";
            }
        }
        return json{{"ok", result.ok}, {"output", output}, {"summary", result.summary},
                    {"detail", result.detail}, {"changed", result.changed},
                    {"pictures", pictures}, {"picture", result.picture_path}};
    }
    if (method == "roster.make") {
        // A seat a build makes for a task nobody on the roster fits, on the
        // model set aside for that. Onto the engine's own roster now, so the
        // task can use it; the session is handed a copy to keep.
        const std::string name  = format::trim(params.value("name", ""));
        const std::string blurb = format::trim(params.value("blurb", ""));
        // Read and written under the lock in one go: two agents making the
        // same seat at once must make one seat.
        std::unique_lock<std::mutex> configuring(config_mutex_);
        if (config_.build.worker_model.empty()) {
            return json{{"ok", false}, {"error", "no model is set for new agents -- Settings, Build"}};
        }
        if (name.empty()) {
            return json{{"ok", false}, {"error", "a seat needs a name"}};
        }
        if (const std::optional<std::size_t> at = config_.roster.find(name)) {
            // Already there, by that name: it is the seat, when it can answer.
            const Expert seat = config_.roster.at(*at);
            const bool   able = config_.has_expert(seat.id);
            const ModelParams seated = config_.expert(seat.id);
            return json{{"ok", able}, {"id", seat.id}, {"name", seat.name}, {"tag", seat.tag},
                        {"keywords", seat.keywords}, {"remote", seated.remote()},
                        {"local", seated.remote() ? std::string() : seated.path},
                        {"error", able ? std::string() : seat.name + " is on the roster with no model"}};
        }
        Expert expert;
        expert.name   = name;
        expert.blurb  = blurb.empty() ? name : blurb;
        expert.origin = "build";
        Config      edited = config_;
        std::string error;
        if (!edited.roster.add(expert, error)) {
            return json{{"ok", false}, {"error", error}};
        }
        const ExpertId id = make_expert_id(name);
        edited.experts[id] = params_for(edited, edited.build.worker_model, edited.build.worker_provider);
        edited.resolve_models();
        const Expert      made   = edited.roster.at(*edited.roster.find(id));
        const ModelParams seated = edited.expert(id);
        config_ = std::move(edited);
        hub_.adopt(config_);
        const Config now = config_;
        configuring.unlock();
        state_.configure_seats(now);
        {
            const std::lock_guard<std::mutex> lock(written_mutex_);
            made_seats_.push_back({made, seated});
        }
        if (wake_) {
            wake_();
        }
        return json{{"ok", true}, {"id", id}, {"name", made.name}, {"tag", made.tag},
                    {"keywords", made.keywords}, {"remote", seated.remote()},
                    {"local", seated.remote() ? std::string() : seated.path}};
    }

    // --- what this machine has -------------------------------------------------
    if (method == "machine.facts") {
        // Which of the programs a build reaches for are on PATH, so the
        // architect plans with `python3` on a Mac that has no `python` and
        // an agent does not spend rounds finding that out. Presence only:
        // asking each for its version is a subprocess apiece.
        json present = json::array();
        for (const char* program : {"python3", "python", "pip3", "pip", "pytest", "node", "npm", "npx",
                                    "yarn", "pnpm", "deno", "bun", "git", "gh", "cargo", "rustc", "go",
                                    "java", "javac", "mvn", "gradle", "dotnet", "ruby", "gem", "php",
                                    "composer", "swift", "make", "cmake", "ninja", "gcc", "clang", "cc",
                                    "docker", "sqlite3", "psql", "mysql", "curl", "wget", "zip", "tar"}) {
            if (util::on_path(program)) {
                present.push_back(program);
            }
        }
#if defined(_WIN32)
        const char* os = "Windows";
        const char* shell = "cmd (RUN powershell: for PowerShell)";
#elif defined(__APPLE__)
        const char* os = "macOS";
        const char* shell = "/bin/sh (zsh and bash are there too)";
#else
        const char* os = "Linux";
        const char* shell = "/bin/sh (bash is there too)";
#endif
        return json{{"os", os}, {"shell", shell}, {"programs", present}};
    }

    // --- version control, for a build's commits ------------------------------
    if (method == "git.ready") {
        return json{{"git", tools::git::available()}, {"gh", tools::git::gh_available()},
                    {"repo", tools::git::is_repo(cook_workshop_.root)}};
    }
    if (method == "git.init") {
        const std::string error = tools::git::init(cook_workshop_.root);
        return json{{"ok", error.empty()}, {"error", error}};
    }
    if (method == "git.commit") {
        // Only the paths named, when any are: with agents working side by
        // side, what one finished is committed without what another is
        // halfway through writing.
        std::vector<std::string> paths;
        for (const json& path : params.value("paths", json::array())) {
            if (path.is_string() && tools::resolve_in_root(cook_workshop_.root, path.get<std::string>())) {
                paths.push_back(path.get<std::string>());
            }
        }
        std::string summary;
        const std::string error =
            tools::git::commit(cook_workshop_.root, params.value("message", ""), summary, paths);
        return json{{"ok", error.empty()}, {"summary", summary}, {"error", error}};
    }

    // --- what a build's agents did, kept for teaching ------------------------
    if (method == "teach.record") {
        // One line of JSON per record, per expert, in the project's own
        // history folder: the task an agent was given and what it made, with
        // the files as they are now. What a local expert could be fine-tuned
        // on so that the next build needs the provider less. Nothing leaves
        // the machine; nothing is read back here.
        if (project_dir_.empty()) {
            return json::object();
        }
        const std::string expert = params.value("expert", "");
        if (expert.empty()) {
            return json::object();
        }
        // As a conversation: the task as the question, and the answer as what
        // the agent said with the files it made written out as fenced blocks
        // -- which is the record the trainer reads through the base model's
        // own chat template, so a local expert taught on it answers a task
        // the way the provider's model did, files and all.
        std::string answer = params.value("completion", "");
        json files = json::array();
        for (const json& path : params.value("files", json::array())) {
            if (!path.is_string()) {
                continue;
            }
            const std::optional<std::filesystem::path> file =
                tools::resolve_in_root(cook_workshop_.root, path.get<std::string>());
            std::ifstream in(file ? *file : std::filesystem::path(), std::ios::binary);
            if (!file || !in) {
                continue;
            }
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (content.size() > 16000) {
                content.resize(16000);
            }
            content = crucible::detail::scrub_utf8(content);
            files.push_back(path.get<std::string>());
            answer += "\n\nWRITE: " + path.get<std::string>() + "\n```\n" + content + "\n```";
        }
        json record{{"expert", expert},
                    {"when", static_cast<std::int64_t>(std::time(nullptr))},
                    {"files", std::move(files)},
                    {"messages", json::array({
                        json{{"role", "user"}, {"content", params.value("prompt", "")}},
                        json{{"role", "assistant"}, {"content", crucible::detail::scrub_utf8(answer)}},
                    })}};
        std::error_code ec;
        const std::filesystem::path dir = project_dir_ / "teach";
        std::filesystem::create_directories(dir, ec);
        std::ofstream out(dir / (expert + ".jsonl"), std::ios::app);
        if (out) {
            out << record.dump(-1, ' ', false, json::error_handler_t::replace) << '\n';
        }
        return json::object();
    }
    if (method == "edit.ask") {
        // Asked as the file the change would leave: a WRITE's contents, or an
        // EDIT's blocks applied. An EDIT that would not apply is not put to
        // the person -- it fails with its reason when it runs.
        tools::ToolCall call = call_from(params);
        if (call.kind == tools::ToolKind::Edit) {
            std::string why;
            const std::optional<std::string> after = tools::edited_contents(call, cook_workshop_, why);
            if (!after) {
                return json{{"approved", true}};
            }
            call.content = *after;
        }
        call.kind = tools::ToolKind::Write;
        state_.set_mood(Mood::Idle, "waiting on you: " + call.argument);
        return json{{"approved", ask_about_edit(call, cook_workshop_, cancel)}};
    }

    // --- the journal and the person ---------------------------------------
    if (method == "cook.publish") {
        // The orchestrator owns the cook as it runs; this is where the window
        // and History see it. Saved after every step rather than at the end.
        const std::lock_guard<std::mutex> lock(cook_mutex_);
        const std::string kind = cook_.kind;
        cook_ = cook_from_json(params.value("cook", json::object()), cook_.id);
        if (!params.value("cook", json::object()).contains("kind")) {
            cook_.kind = kind;   // a cook's journal does not say; the engine knows
        }
        publish_cook();
        if (cook_log_) {
            std::string error;
            cook_log_->save(cook_, error);
        }
        return json::object();
    }
    if (method == "cook.await_answer") {
        // One question at a time: the journal holds one, the window shows
        // one, and the answer typed is to that one.
        const std::lock_guard<std::mutex> one_at_a_time(asking_mutex_);
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
                     std::optional<ExpertId> pinned, const std::string& kind) {
    {
        const std::lock_guard<std::mutex> lock(cook_answer_mutex_);
        cook_answer_.reset();
    }
    const Config config = this->config();

    {
        const std::lock_guard<std::mutex> lock(cook_mutex_);
        cook_                = Cook{};
        cook_.id             = CookLog::new_id();
        cook_.goal           = goal;
        cook_.kind           = kind;
        cook_.state          = CookState::Working;
        cook_.budget_seconds = budget_seconds;
        cook_.started_unix   = static_cast<std::int64_t>(std::time(nullptr));
        for (const attach::Attachment& one : attachments) {
            cook_.attachments.push_back(attach::tile_of(one));
        }
        publish_cook();
    }

    cook_workshop_ = workshop_for(root);
    cook_search_   = tools::SearchSettings{};
    cook_search_.enabled         = config.tools.web_search;
    cook_search_.provider        = config.tools.search_provider;
    cook_search_.endpoint        = config.tools.search_endpoint;
    cook_search_.api_key         = config.tools.search_api_key;
    cook_search_.max_results     = config.tools.search_results;
    cook_search_.timeout_seconds = config.tools.search_timeout;
    cook_attachments_ = std::move(attachments);

    json request = routing_request(goal, pinned);
    std::string id;
    json        tiles;
    {
        const std::lock_guard<std::mutex> lock(cook_mutex_);
        id    = cook_.id;
        tiles = cook_to_json(cook_)["attachments"];
    }
    request["context"]         = "build";
    request["id"]              = id;
    request["goal"]            = goal;
    request["root"]            = root.string();
    request["budget_seconds"]  = budget_seconds;
    request["attachments"]     = tiles;
    request["has_attachments"] = !cook_attachments_.empty();
    request["system_prompt"]   = config.system_prompt;
    request["tools"]           = tools::workshop_instructions(cook_workshop_, tools::ToolAudience::Cook);
    const bool building = kind == "build";
    if (building) {
        request["directive"] = goal;
        request["settings"]  = json{{"architect", config.build.architect},
                                    {"can_make", !config.build.worker_model.empty()},
                                    {"auto_commit", config.build.auto_commit},
                                    {"confirm_plan", config.build.confirm_plan},
                                    {"rounds_per_task", config.build.rounds_per_task},
                                    {"agents", config.build.agents},
                                    {"split", config.build.split}};
    }

    std::string error;
    if (wait_for_python(error)) {
        try {
            orchestra_.call(building ? "build.run" : "cook.run", request);
        } catch (const std::exception& e) {
            error = std::string("the build stopped: ") + e.what();
        }
    }

    // However it ended, the journal is finished and kept.
    {
        const std::lock_guard<std::mutex> lock(cook_mutex_);
        if (!error.empty() || cook_.state == CookState::Working || cook_.state == CookState::Asking
            || cook_.state == CookState::Finishing) {
            cook_.state = error == "stopped" || cook_cancel_.load(std::memory_order_relaxed)
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
        if (!busy_.load(std::memory_order_relaxed)) {
            // The status line is the chat's while a turn is being answered.
            state_.set_linked(std::nullopt);
            if (cook_.state == CookState::Failed && cook_.steps.empty()) {
                state_.set_mood(Mood::Error, cook_.outcome);   // it never started
            } else {
                state_.set_mood(Mood::Idle, (building ? "build finished -- " : "cook finished -- ")
                                                + cook_.headline());
            }
        }
    }
    cook_attachments_.clear();
    cook_stop_.store(false, std::memory_order_relaxed);
    if (wake_) {
        wake_();
    }
}

}  // namespace crucible
