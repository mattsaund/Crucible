// SPDX-License-Identifier: MIT
// The delegation loop, running on its own thread.
//
// Everything llama.cpp touches lives here. The UI hands the engine a prompt and
// gets told, via AppState plus a wake callback, how the delegation is going.
// The engine blocks for seconds at a time loading a 30B expert; keeping it off
// the UI thread is what lets the window keep drawing while that happens.
//
// A chat turn is asked for by the worker, and its loop is the orchestrator's.
// The API server and a benchmark run ask for models at the same time as the
// chat: a provider's model is asked in parallel, a model on this machine is
// held by a lease while somebody has it and answers one caller at a time. See
// leases.hpp for why a model in use is never freed under its user.
#pragma once

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <filesystem>

#include "crucible/config/config.hpp"
#include "crucible/engine/leases.hpp"
#include "crucible/llm/mlx_delegator.hpp"
#include "crucible/llm/mlx_server.hpp"
#include "crucible/llm/model_host.hpp"
#include "crucible/llm/remote_model.hpp"
#include "crucible/orchestra/link.hpp"
#include "crucible/routing/router.hpp"
#include "crucible/engine/state.hpp"
#include "crucible/serve/server.hpp"
#include "crucible/tools/attachments.hpp"
#include "crucible/tools/mcp.hpp"
#include "crucible/tools/processes.hpp"
#include "crucible/tools/workshop.hpp"

namespace crucible {

class Engine {
public:
    /// `wake` is called whenever the state changed and the screen should be
    /// redrawn. It must be safe to call from a non-UI thread.
    Engine(Config config, AppState& state, std::function<void()> wake);
    ~Engine();
    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

    /// Start the worker and load the router model. Returns immediately.
    void start();

    /// Stop the worker, canceling any generation in flight.
    void stop();

    /// Start the orchestrator now rather than with the first prompt, which
    /// otherwise waits for Python to start before anything else. Does
    /// nothing while Crucible's Python is not installed, or once it is up;
    /// a start that fails is said by the first call that needs it. Blocks
    /// while Python starts, so it is for a thread that can wait.
    void prepare_orchestrator();

    /// Queue a prompt. `pinned` skips routing and sends it straight to that
    /// expert, which is how `/physics ...` works.
    ///
    /// `attachments` are read when the expert is known, not before: how much
    /// of a long document fits is a question about that expert's context.
    ///
    /// One that reaches the worker while a benchmark runs is not answered: its
    /// turn fails, and says why. See benching.
    void submit(std::string prompt, std::optional<ExpertId> pinned = std::nullopt,
                std::vector<attach::Attachment> attachments = {});

    /// Ask the current generation to stop at the next token boundary.
    void cancel();

    /// Drop the resident expert, freeing its memory without exiting.
    void release_expert();

    /// Drop every model Crucible has in memory: the expert and the delegator.
    ///
    /// Not release_expert twice over. With the delegator set to stay resident
    /// it is the larger of the two on plenty of machines, and releasing only
    /// the expert leaves the card with a model on it that nothing is about to
    /// use -- which is exactly the state somebody hits Eject to get out of,
    /// usually because they want the memory for something that is not Crucible.
    ///
    /// Whatever is needed comes back on the next prompt.
    void release_all();

    /// Where the box sends prompts from now on: one expert, or the delegator
    /// (nullopt). With `load`, that one is loaded now, so the first prompt
    /// to it does not wait for it. With the delegator on demand, an expert
    /// chosen here also stays loaded between prompts while the delegator
    /// stays away: nothing is routed while it is chosen, and the two
    /// together would leave the expert less of the cards. Said again with
    /// every prompt, without `load`; only a change stops a load under way.
    void choose(std::optional<ExpertId> seat, bool load);

    /// Drop every loaded model and load the router again.
    ///
    /// For when the hardware under Crucible changed: a model picks its devices
    /// once, when it loads, so a runtime installed from the settings screen
    /// does nothing for the model that is already resident. This is what makes
    /// a new GPU backend take effect without restarting.
    void reload_models();

    /// Forget the conversation history sent to experts.
    void reset_history();

    /// Replace that history wholesale, which is what resuming a stored
    /// conversation needs: the expert has to see what is already on screen or
    /// it will answer the next question with no idea what came before.
    void restore_history(std::vector<ChatMessage> history);

    /// Which project this session is about: the directory being worked in.
    ///
    /// Set once at startup rather than passed with each request. Which project
    /// a turn belongs to is a property of the session -- Crucible is started
    /// inside a directory and is about that directory -- not of the prompt.
    ///
    /// `root` also decides whether an expert can touch the disk at all. It is
    /// only ever set to a folder the user has trusted, so having one *is* the
    /// permission: there is no second switch behind it. Pass an empty path and
    /// the workshop verbs are not offered and would be refused if they were.
    void set_project(std::filesystem::path root);

    /// Programs experts have left running: START's processes. The window
    /// lists them and can stop one.
    tools::Processes& processes() { return processes_; }

    /// The MCP servers whose tools experts are offered. See tools/mcp.hpp.
    tools::mcp::Hub& mcp() { return mcp_; }

    /// What each frontier model has been asked, from any thread. See spend.hpp.
    std::vector<spend::Model> spending() const { return hub_.spending(); }

    /// Answer the edit a chat turn is waiting on: true writes the file, false
    /// leaves it alone and tells the expert so.
    ///
    /// Only reachable while a snapshot carries a PendingEdit. Calling it at any
    /// other time is harmless -- the answer is dropped, because there is nobody
    /// waiting to read it.
    void approve_edit(bool approved);

    // --- benchmarks, in engine_bench.cpp -----------------------------------

    /// Run models through the benchmark suite. `request` is what the Bench
    /// tab asked for -- which model files, which benchmarks, what size --
    /// and goes to the orchestrator's bench.run, which asks for each item and
    /// checks each answer. Refused while a run or a reply is going: both want
    /// the cards. Returns why it was refused, or nothing.
    std::string start_bench(nlohmann::json request);

    /// Stop the run: the item in progress is abandoned and what was finished
    /// is kept, marked as stopped.
    void stop_bench();

    /// True while a run goes. A chat turn is refused meanwhile, and nothing
    /// is loaded for one: the run has the cards to itself, and what it times
    /// is its model alone.
    bool benching() const { return benching_.load(std::memory_order_relaxed); }

    /// Ask the delegator to write worked examples for a seat that has none.
    ///
    /// Two example questions per expert are worth seven points of routing
    /// accuracy on the benchmark (89% to 96%), and they are the one thing
    /// New expert cannot ask a person for: a description is something you can
    /// write about your own field, two questions phrased the way a delegator
    /// needs them is not. So the delegator writes its own.
    ///
    /// Queued like any other request, so it runs between prompts and never
    /// competes with one. Silently does nothing when there is no delegator
    /// loaded -- the seat still routes on its blurb and its keywords, just less
    /// sharply, and a machine with no delegator has bigger gaps than this.
    void write_examples(ExpertId id);

    /// Examples the delegator has written since this was last called, and
    /// clears them.
    ///
    /// An outbox rather than a callback: the engine produces these on its
    /// worker thread, and folding them into the config means touching the
    /// settings screen's copy, which belongs to the UI thread. The UI drains
    /// this when it is woken, the same way it drains a finished runtime build.
    std::vector<std::pair<ExpertId, std::vector<std::string>>> take_written_examples();

    /// Name a conversation: what it is about, in two to four words -- "Math
    /// homework" -- written by the delegator from `excerpt`, its first
    /// exchange. Queued like write_examples, so it runs between prompts and
    /// never competes with one; with no delegator, the first words of the
    /// first prompt stand in. The name comes back through take_session_names.
    ///
    /// `spare` is for one named in passing, from before names existed: only
    /// a delegator already in memory names it, and nothing does when there
    /// is none -- it is listed by its first words already.
    void name_session(std::string session, std::filesystem::path root, std::string excerpt,
                      bool spare = false);

    struct SessionName {
        std::string           session;
        std::filesystem::path root;   ///< the project it belongs to
        std::string           name;   ///< empty when none could be written
    };

    /// What naming has come to since this was last called, and clears it:
    /// a name, or none, for each conversation asked about.
    std::vector<SessionName> take_session_names();

    /// Replace the running configuration, as the settings screen does.
    ///
    /// Applied on the worker thread between requests, never mid-generation.
    /// A changed router model is reloaded; an expert whose model changed is
    /// evicted so the next prompt picks up the new file.
    void apply_config(Config config);

    /// A snapshot of the configuration currently in force.
    Config config() const;

    bool is_busy() const { return busy_.load(std::memory_order_relaxed); }

    /// The API server: whether it listens and where, what it serves, and what
    /// it has answered. It listens as the configuration says from start(),
    /// and follows apply_config() at once rather than after the queue. See
    /// serve/server.hpp.
    serve::Status server_status() const;

private:
    /// One unit of work for the engine thread. `kind` keeps config changes and
    /// expert releases on the same queue as prompts, so they are applied in
    /// order and never race with a generation in flight.
    enum class RequestKind { Prompt, ReleaseExpert, ReleaseAll, ReloadModels,
                             ApplyConfig, WriteExamples, NameSession };

    struct Request {
        RequestKind             kind = RequestKind::Prompt;
        std::string             prompt;
        std::optional<ExpertId> pinned;
        Config                  config;
        ExpertId                expert;  ///< for WriteExamples

        // for Prompt
        std::vector<attach::Attachment> attachments;

        // for NameSession, with `prompt` (the excerpt)
        std::filesystem::path root;
        std::string           session;
        bool                  spare = false;
    };

    void run();

    /// A prompt: the turn opened in the transcript and handed to the
    /// orchestrator, which answers it. In engine_chat.cpp.
    void handle(const Request& request);

    /// Load the delegator model, when one is set. A failure is said on the
    /// delegator's row, and routing goes on keywords until it loads. `cancel`
    /// is whose Stop ends the load: the chat's when none is given.
    void load_router(const CancelCallback& cancel = {});

    /// Have the delegator loaded and waiting, whichever mode it is in.
    ///
    /// "Load on demand" decides when the delegator is *freed* -- when an
    /// expert needs its room -- not when it comes back. It comes back as soon
    /// as the work is over, so that the next prompt is routed the instant it
    /// is sent rather than after a load nobody asked to wait for: beside the
    /// expert when they both fit, on the processor when they do not and that
    /// is allowed, and in the expert's place otherwise. One on the processor
    /// goes back to the cards once they have room for it again.
    void ready_delegator();

    /// Put the table back after a prompt, however it ended: the chat's own
    /// Stop is cleared, and then tidy_models.
    void settle();

    /// With the delegator on demand: bring it back, and stop an MLX model
    /// nobody is using. Experts stay while there is room for them: the next
    /// prompt is as likely as not for the same one. Called once a prompt or
    /// a benchmark is over, so the window is free to take the next prompt
    /// while it happens -- the prompt simply waits its turn behind the load.
    /// Any thread.
    void tidy_models();

    /// Make sure the delegator is loaded: beside the experts, on the
    /// processor, or in the place of one nobody is using, as
    /// ModelHost::acquire_router decides. A no-op when it is already there,
    /// or none is set. Call with host_mutex_ held. `cancel` as load_router's.
    void ensure_router(const CancelCallback& cancel = {});

    /// load_router for a delegator that is an MLX folder, which starts its
    /// helper once there is room for it. See mlx_delegator.hpp.
    void load_mlx_router(const ModelParams& router, const CancelCallback& cancel);

    /// Whether the delegator set is loaded, a GGUF here or an MLX model in
    /// its helper. With host_mutex_ held.
    bool delegator_here() const;

    /// delegator.format, .score and .generate answered by the MLX helper.
    nlohmann::json serve_mlx_delegator(const std::string& method, const nlohmann::json& params,
                                       const CancelCallback& cancel);

    /// Drop the delegator: between routes when "keep delegator loaded" is
    /// off, and in either mode for Eject, a changed runtime and changed
    /// loading settings. Call with host_mutex_ held.
    void release_router();

    /// release_router() for where the delegator is freed on its own -- an
    /// expert about to load -- which must not do it in the middle of somebody
    /// else's route. Takes host_mutex_; call without it.
    void release_router_between_routes();

    /// With the delegator on demand, release_router_between_routes() when
    /// `expert` is about to be loaded and needs the room the delegator holds
    /// (ModelHost::needs_router_room); nothing when the two fit side by side,
    /// or the expert is a provider's. `waiting` is called first when a route
    /// is using the delegator and is to be waited for. Takes host_mutex_;
    /// call without it.
    void make_room_for(const ModelParams& expert, const std::function<void()>& waiting = {});
    void do_apply_config(Config config);
    void do_write_examples(const ExpertId& id);

    /// The model file of the expert chosen in the box (see choose()), when
    /// one is chosen and it is a model on this machine; else empty.
    std::string chosen_path() const;

    /// Whether an expert is chosen in the box, a provider's included: then
    /// nothing is routed, and the delegator has no business being loaded.
    bool seat_chosen() const;
    mutable std::mutex      chosen_mutex_;
    std::optional<ExpertId> chosen_;

    /// Loads for choose(), on a thread of their own rather than in the
    /// queue: a prompt sent while one runs is on the screen at once, waiting
    /// on the load instead of hidden behind it. The latest choice wins, and
    /// one made while an earlier is loading stops that load. The worker
    /// joins it before the models go.
    void prepare_loop();
    void do_prepare(const std::optional<ExpertId>& seat, std::uint64_t choice);
    std::thread                prepare_thread_;
    std::mutex                 prepare_mutex_;
    std::condition_variable    prepare_wanted_;
    std::optional<ExpertId>    prepare_seat_;        ///< under prepare_mutex_
    std::uint64_t              prepare_asked_ = 0;   ///< under prepare_mutex_: loads asked for
    std::atomic<std::uint64_t> choices_{0};          ///< how many times the choice has changed
    void do_name_session(const Request& request);

    // --- the orchestrator ----------------------------------------------------
    //
    // Routing, the loop a prompt is worked in and the benchmarks are Python,
    // in a process of their own: see orchestra/link.hpp. They ask the core for
    // what only the core may do, and it answers here, on threads of the
    // link's own: see serve().

    /// Answer one of the orchestrator's requests. Throws with the reason when
    /// it cannot be answered, which goes back to the orchestrator as an error.
    ///
    /// Called on a thread of the link's own, one per request, so a chat turn
    /// and a benchmark run are answered side by side. Whose a request is --
    /// the chat's or the benchmark's -- is in its "context", and it decides
    /// which Stop the request heeds.
    nlohmann::json serve(const std::string& method, const nlohmann::json& params);

    /// What the orchestrator needs to route: the prompt, the roster as the
    /// delegator is shown it, which seats have a model, and the policy, all
    /// as `config` has them.
    static nlohmann::json routing_request(const Config& config, const std::string& prompt,
                                          const std::optional<ExpertId>& pinned);

    /// A tool call as the orchestrator sends one back: the fields tools.parse
    /// handed it, kind by name.
    static tools::ToolCall call_from(const nlohmann::json& params);

    /// tools.parse: the call an expert's reply makes, if any, and what it
    /// asked to look up. Any thread.
    static nlohmann::json parse_reply(const nlohmann::json& params);

    /// Have Crucible's Python, waiting for it while it is being downloaded.
    /// False, with `error` saying why, when it is not coming.
    bool wait_for_python(std::string& error, const CancelCallback& cancel = {});

    orchestra::Link orchestra_;

    /// The Stop a request from the orchestrator heeds: the benchmark's for
    /// one of a run's, the chat's for anything else.
    CancelCallback cancel_for(const nlohmann::json& params) const;

    /// Attachments as a message's text and pictures, sized to `model`:
    /// `share` of its context, less what `messages` already take.
    attach::Composed read_attachments(std::vector<attach::Attachment> attachments,
                                      const ChatModel& model,
                                      const std::vector<ChatMessage>& messages, double share);

    /// The workshop, pointed at `root`: every tool the trusted folder allows.
    ///
    /// Asking a question about a file and asking for it to be changed are the
    /// same permission, and a chat that could read but not write was a
    /// distinction nobody asked for. An empty root gives a disabled workshop,
    /// which is what an untrusted folder produces.
    tools::WorkshopSettings workshop_for(const std::filesystem::path& root) const;

    /// A model, held: the model to ask, and the lease that keeps it in
    /// memory for as long as this is alive. A provider's model needs no
    /// lease; a local one cannot be freed while one is held.
    struct HeldModel {
        ChatModel*     model = nullptr;
        Leases::Lease  lease;
    };

    /// Programs experts have started and left running. Stopped with the engine.
    tools::Processes                processes_;

    /// The MCP servers, and the thread that starts them: a server is a
    /// program that may be fetched by npx on its first run, and nothing
    /// should wait for that but the call that needs it. See warm_mcp().
    tools::mcp::Hub                 mcp_;
    std::thread                     mcp_thread_;
    std::mutex                      mcp_mutex_;   ///< warm_mcp is called from two threads
    bool                            mcp_warming_ = false;   ///< under mcp_mutex_
    bool                            warm_again_  = false;   ///< under mcp_mutex_: a newer config came

    /// Hand the MCP servers in `config` to the hub and start them, on
    /// mcp_thread_. Any thread, and never waits for a warm-up already going:
    /// that one goes round again for the newer configuration.
    void warm_mcp(const Config& config);

    /// A benchmark run's half of serve(): the model under test, its replies,
    /// the datasets, the results. See engine_bench.cpp.
    nlohmann::json serve_bench(const std::string& method, const nlohmann::json& params);
    void do_bench(nlohmann::json request);

    /// How many questions the Bench asks `model` at once: as many as the
    /// cards hold a cache of `context` tokens for beside its weights.
    int bench_at_once(const ModelParams& model, int context);

    // --- a chat turn, in engine_chat.cpp -------------------------------------
    //
    // The turn's loop -- the route, the rounds, the tools between them, how it
    // ends -- is the orchestrator's: see scripts/orchestrator's chat.py. The
    // worker opens the turn and asks for it; what the core keeps for it
    // between the calls the orchestrator makes is a ChatTurn.
    struct ChatTurn;

    /// The turn being answered, or nothing between turns. One at a time: the
    /// chat's turns are the worker's, and it asks for one after another.
    std::shared_ptr<ChatTurn> chat_turn_;
    std::mutex                chat_mutex_;   ///< guards chat_turn_

    /// The chat turn's half of serve(): its seat, its conversation, its
    /// rounds, its tools and what its transcript says.
    nlohmann::json serve_turn(const std::string& method, const nlohmann::json& params);

    /// The model behind `id`, held and ready to be asked: loaded onto the
    /// cards if it is a file, looked up if it is a provider's. No model, with
    /// `error` set, when it could not be had; `error` is "stopped" when
    /// `cancel` stopped a load or a wait.
    ///
    /// The one place that knows there are two kinds. When a local model will
    /// not fit beside the ones other seats are holding, this waits for one of
    /// them to be let go, and says so on the status line -- and to `waiting`,
    /// when given. `load_ms` is how long the swap took, and 0 when there was
    /// none.
    HeldModel hold_model(const ExpertId& id, const ModelParams& params, const std::string& name,
                         const CancelCallback& cancel, long& load_ms, std::string& error,
                         const std::function<void()>& waiting = {});

    /// hold_model's two halves, with host_mutex_ held: an MLX model brought
    /// up in its server, or a GGUF loaded onto the cards. Null with
    /// `crowded` set when the models in use leave it no room.
    ChatModel* serve_mlx(const ExpertId& id, const ModelParams& params, const std::string& name,
                         const CancelCallback& cancel, long& load_ms, std::string& error,
                         bool& crowded);
    ChatModel* load_gguf(const ExpertId& id, const ModelParams& params, const std::string& name,
                         const CancelCallback& cancel, long& load_ms, std::string& error,
                         bool& crowded);

    /// What the host must not free: every model somebody holds a lease on.
    ModelHost::Keep in_use() const;

    /// Guards host_ and the MLX server: a load, a free, the delegator's
    /// scoring. Never held while an expert generates -- a lease keeps that
    /// model in memory, and the model takes its own turns.
    std::mutex host_mutex_;

    /// Routes under way. A route is several calls -- delegator.ready, then a
    /// format and a score for each order -- and each takes host_mutex_ on its
    /// own, so the delegator freed between two of them failed the route:
    /// delegator.ready opens one, delegator.done closes it, and the automatic
    /// frees wait for them. Changed under host_mutex_. `routes_touched_ms_`
    /// is when a route last said anything, so one that never closed -- an
    /// orchestrator that died halfway -- stops counting after a minute.
    std::atomic<int>          routes_open_{0};
    std::atomic<std::int64_t> routes_touched_ms_{0};
    std::mutex                routes_mutex_;
    std::condition_variable   routes_done_;

    /// Which local models are in use. See leases.hpp.
    Leases leases_;

    Config                 config_;
    mutable std::mutex     config_mutex_;   ///< guards config_ against the UI thread
    AppState&              state_;
    std::function<void()>  wake_;

    std::unique_ptr<ModelHost> host_;
    /// Set once the worker has made host_: a benchmark's thread can start
    /// before it has, and waits.
    std::atomic<bool>          host_ready_{false};

    /// The experts that are not on this machine. Nothing in it is resident in
    /// the sense `host_` means -- there are no weights -- so it sits beside
    /// the host rather than inside it: a seat is answered by one or the other,
    /// and asking a provider never evicts what is loaded here.
    ///
    /// Asked from the chat's thread and the API server's, and counting what
    /// every one of them costs. See remote_model.hpp.
    remote::Hub                hub_;

    /// An MLX model's server, when the seat with the turn is one, and which
    /// seat that is. One model in memory at a time holds for these too: a
    /// GGUF taking the seat stops it, and it stops whatever GGUF was here.
    /// See mlx_server.hpp.
    mlx::Server                mlx_;
    std::optional<ExpertId>    mlx_seat_;

    /// Stop the MLX server, and put its seat back to dormant. With
    /// host_mutex_ held, and only when nobody holds a lease on its model.
    void stop_mlx();

    /// The delegator, when it is an MLX model. It runs in a helper of its own,
    /// since MLX's server cannot score, started and stopped with host_mutex_
    /// held where a GGUF delegator is loaded and let go.
    mlx::Delegator             mlx_delegator_;

    /// Tell the state what is in memory now: the delegator, the resident
    /// experts, an MLX model's server. With host_mutex_ held, after anything
    /// that loads or lets go of one.
    void show_loaded();

    /// The delegator file that last failed to load, so that ready_delegator
    /// does not try it again after every prompt. Empty when it did not fail.
    std::string                router_failed_for_;

    std::vector<ChatMessage> history_;

    /// Pictures as the window sent them, by path: shrunk to what a provider
    /// takes. Asking a turn again sends the same picture rather than reading
    /// the original, which may be too large to send. The last few only.
    std::vector<std::pair<std::string, attach::Image>> pictures_;
    std::mutex                                         pictures_mutex_;

    /// See take_written_examples. Guarded by its own mutex rather than by
    /// `mutex_`, which the worker holds while it waits for work.
    std::mutex written_mutex_;
    std::vector<std::pair<ExpertId, std::vector<std::string>>> written_examples_;
    std::vector<SessionName> session_names_;   ///< under written_mutex_ too

    /// The directory this session is about, and the one thing that decides
    /// whether an expert may touch the disk.
    ///
    /// Set from the UI thread by set_project and read by the worker, both only
    /// between requests -- a project cannot change while a turn is running,
    /// which is enforced above this by refusing to open one then.
    std::filesystem::path        project_root_;

    /// The benchmark run, on its own thread, and the one model it holds:
    /// the model being tested, and the answers it is giving. See
    /// engine_bench.cpp.
    struct BenchModel;
    std::thread                  bench_thread_;
    std::atomic<bool>            benching_{false};
    std::atomic<bool>            bench_stop_{false};
    std::mutex                   bench_mutex_;
    std::shared_ptr<BenchModel>  bench_model_;

    /// Ask the user about one edit and block until they answer. True means
    /// write it.
    ///
    /// False when the edit was declined, and also when the turn was canceled
    /// or the engine is shutting down while parked here -- all three mean "do
    /// not write the file", which is the only question the caller has.
    /// `cancel` is the Stop that ends the wait.
    bool ask_about_edit(const tools::ToolCall& call, const tools::WorkshopSettings& workshop,
                        const CancelCallback& cancel);

    /// The answer to an edit, handed across from the UI thread. Kept only
    /// while one is being asked: see approve_edit.
    std::mutex                   edit_mutex_;
    std::condition_variable      edit_answered_;
    std::optional<bool>          edit_approved_;
    bool                         edit_asking_ = false;

    std::thread             worker_;
    std::mutex              mutex_;
    std::condition_variable queued_;
    std::deque<Request>     pending_;
    std::atomic<bool>       running_{false};
    std::atomic<bool>       cancel_{false};
    std::atomic<bool>       busy_{false};

    /// Whether a write may go ahead without asking. A copy of
    /// `config_.tools.auto_edits` that the window sets the moment the Auto
    /// button is pressed: a configuration change is queued behind whatever is
    /// running, and a prompt can be many minutes of running.
    std::atomic<bool>       auto_edits_{false};

    // --- the API server, in engine_serve.cpp ----------------------------------
    //
    // The engine is what the server asks: serve::Backend's two questions,
    // answered with the delegator or a model on this machine, held as a
    // seat holds one.

    class Served final : public serve::Backend {
    public:
        explicit Served(Engine& engine) : engine_(engine) {}
        std::string   model() const override { return served_id(engine_.config()); }
        std::string   embedder() const override { return embedder_id(engine_.config()); }
        serve::Answer answer(const serve::Ask& ask, const serve::Sink& sink) override {
            return engine_.serve_request(ask, sink);
        }

    private:
        Engine& engine_;
    };

    /// A request from the server, answered. On the server's own thread.
    serve::Answer serve_request(const serve::Ask& ask, const serve::Sink& sink);

    /// An embeddings request, answered by the model it names: the one chosen
    /// for embeddings, held as the served model is, or the model served.
    serve::Answer serve_embeddings(const serve::Ask& ask, const serve::Sink& sink, const Config& config,
                                   const CancelCallback& cancel);

    /// The file embeddings come from when one is chosen apart from the model
    /// served, and what the server calls it: that file's name, or the
    /// served id when none is chosen.
    static std::string embedder_file(const Config& config);
    static std::string embedder_id(const Config& config);

    /// The seat a model the server serves is held under, and what the
    /// right-hand panel says has it. Capitalized, which no seat's id is.
    static constexpr const char* kServerSeat = "Server";

    /// Listen as `config` says, or stop listening. From start() and
    /// apply_config(), on the thread that calls them.
    void configure_server(const Config& config);

    /// The file the server answers with, as `config` has it: the
    /// delegator's, or the model it names. Empty when there is none, and for
    /// a provider's model, which has no file here.
    static std::string served_file(const Config& config);

    /// What the server calls it: that file's name, or a provider's model's
    /// own name.
    static std::string served_id(const Config& config);

    /// How the served model loads: as the seat that has the same file does,
    /// so one copy in memory answers both, or as every seat starts from.
    /// `seat`, when given, is set to that seat, or to kServerSeat for none.
    static ModelParams served_params(const Config& config, ExpertId* seat = nullptr);

    /// The same, for `file`: the served model's, or the embeddings model's.
    static ModelParams file_params(const Config& config, const std::string& file, ExpertId* seat = nullptr);

    /// A model from the folder, held for a request as a seat holds one:
    /// loaded when it is not in memory, waited for when somebody else is
    /// answering with it. Null, with `out` saying why, when it cannot be had.
    HeldModel hold_served(const ModelParams& params, const ExpertId& holder, const serve::Sink& sink,
                          const CancelCallback& cancel, serve::Answer& out);

    /// Whoever a request waits behind while the models' lock is taken, in
    /// the words Settings shows: "the chat", "loading".
    std::string serve_blocker() const;

    /// One request, answered by `model` with `params` for what the request
    /// left out. `local` is the same model when it is one on this machine,
    /// which answers a conversation in OpenAI's own shape, tools and all,
    /// and a text completion; null otherwise.
    serve::Answer serve_with(ChatModel& model, LoadedModel* local, ModelParams params,
                             const serve::Ask& ask, const serve::Sink& sink, const CancelCallback& cancel);

    /// Last, so they are gone before anything a request is answered from.
    Served                         served_{*this};
    std::unique_ptr<serve::Server> server_ =
        std::make_unique<serve::Server>(served_, [this] {
            if (wake_) {
                wake_();
            }
        });
};

}  // namespace crucible
