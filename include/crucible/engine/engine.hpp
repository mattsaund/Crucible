// SPDX-License-Identifier: MIT
// The delegation loop, running on its own thread -- and a build, on another.
//
// Everything llama.cpp touches lives here. The UI hands the engine a prompt and
// gets told, via AppState plus a wake callback, how the delegation is going.
// The engine blocks for seconds at a time loading a 30B expert; keeping it off
// the UI thread is what lets the window keep drawing while that happens.
//
// A chat turn is asked for by the worker, and its loop is the orchestrator's,
// as a build's is. A build runs on a thread of its own, its agents asking for
// models at the same time as each other and as the chat: a provider's model
// is asked in parallel, a model on this machine is held by a lease while a
// seat has it and answers one caller at a time. See leases.hpp for why a
// model in use is never freed under its user.
#pragma once

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <filesystem>

#include "crucible/config/config.hpp"
#include "crucible/cook/journal.hpp"
#include "crucible/engine/leases.hpp"
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

    /// Queue a prompt. `pinned` skips routing and sends it straight to that
    /// expert, which is how `/physics ...` works.
    ///
    /// `attachments` are read when the expert is known, not before: how much
    /// of a long document fits is a question about that expert's context.
    ///
    /// One that reaches the worker while a benchmark runs is not answered: its
    /// turn fails, and says why. See benching.
    ///
    /// A prompt that asks for software to be made becomes a build (see the
    /// orchestrator's intent.py), unless `answer_only` says it is to be
    /// answered whatever it asks: a model under test in Create is asked
    /// questions, never set to work.
    void submit(std::string prompt, std::optional<ExpertId> pinned = std::nullopt,
                std::vector<attach::Attachment> attachments = {}, bool answer_only = false);

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

    /// Which project this session is about: the directory being worked in, and
    /// the history folder beside it that cooks are journalled to.
    ///
    /// Set once at startup rather than passed with each request. Which project
    /// a turn belongs to is a property of the session -- Crucible is started
    /// inside a directory and is about that directory -- not of the prompt.
    ///
    /// `root` also decides whether an expert can touch the disk at all. It is
    /// only ever set to a folder the user has trusted, so having one *is* the
    /// permission: there is no second switch behind it. Pass an empty path and
    /// the workshop verbs are not offered and would be refused if they were.
    void set_project(std::filesystem::path root, std::filesystem::path project_dir);

    /// Start a cook: work on the project towards `goal`, taking actions rather
    /// than describing them, until the budget runs out or the user stops it.
    ///
    /// `budget_seconds` of 0 means "until I stop it". `root` is the directory
    /// every file the cook touches must be inside -- the project the user
    /// started Crucible in, and one they have already trusted.
    ///
    /// Runs on a thread of its own, not the worker: the chat stays open while
    /// a build works, and its turns are answered beside the build's agents.
    /// One build at a time -- a second is refused, with the reason returned,
    /// and so is one asked for while a benchmark runs.
    ///
    /// `pinned` starts it with that expert rather than whoever the goal routes
    /// to. A HANDOFF still goes through the delegator: the pin is where the
    /// work begins, not who must do all of it.
    ///
    /// `kind` is "cook" or "build". A build is a cook with a plan: the
    /// orchestrator's build loop rather than its cook loop, on the same
    /// journal and the same tools. See scripts/orchestrator's build.py.
    ///
    /// `started`, when given, is told the new journal's id, so the turn that
    /// asked for the build can be marked with it at once (Turn::build). When
    /// a turn is marked so, the build's end is its reply and is remembered by
    /// the conversation, as an answer is.
    std::string start_cook(std::string goal, int budget_seconds, std::filesystem::path root,
                           std::vector<attach::Attachment> attachments = {},
                           std::optional<ExpertId> pinned = std::nullopt,
                           std::string kind = "cook", std::string* started = nullptr);

    /// A seat a build made for itself: see take_made_seats.
    struct MadeSeat {
        Expert      expert;
        ModelParams params;
    };

    /// Seats builds have made since this was last called, and clears them.
    ///
    /// An outbox like take_written_examples, for the same reason: the build
    /// runs on a thread of its own and the seat is already on the engine's
    /// own roster so the next task can use it, but the config file and the
    /// settings screen are the session's, and it folds these in when it is
    /// woken.
    std::vector<MadeSeat> take_made_seats();

    /// Programs experts have left running: START's processes. The window
    /// lists them and can stop one.
    tools::Processes& processes() { return processes_; }

    /// The MCP servers whose tools experts are offered. See tools/mcp.hpp.
    tools::mcp::Hub& mcp() { return mcp_; }

    /// What each frontier model has been asked, from any thread. See spend.hpp.
    std::vector<spend::Model> spending() const { return hub_.spending(); }

    /// Ask the running cook to wrap up.
    ///
    /// Not a kill: the cook stops taking new work, makes a finishing pass to
    /// leave the project in a working state, and writes what it did. That pass
    /// is the whole reason this is different from cancel_cook().
    void stop_cook();

    /// Stop the running build now: every agent's generation, every request
    /// to a provider it is waiting on, every question it is parked on. The
    /// chat is left alone, as cancel() leaves the build alone.
    void cancel_cook();

    /// Answer the question a cook is waiting on, releasing it to carry on.
    void answer_cook(std::string answer);

    /// Answer the edit a chat turn is waiting on: true writes the file, false
    /// leaves it alone and tells the expert so.
    ///
    /// Only reachable while a snapshot carries a PendingEdit. Calling it at any
    /// other time is harmless -- the answer is dropped, because there is nobody
    /// waiting to read it.
    void approve_edit(bool approved);

    /// True from the moment a cook starts until its finishing pass is done.
    bool cooking() const { return cooking_.load(std::memory_order_relaxed); }

    // --- benchmarks, in engine_bench.cpp -----------------------------------

    /// Run models through the benchmark suite. `request` is what the Bench
    /// tab asked for -- which model files, which benchmarks, what size --
    /// and goes to the orchestrator's bench.run, which asks for each item and
    /// checks each answer. Refused while a run, a build or a reply is going:
    /// all three want the cards. Returns why it was refused, or nothing.
    std::string start_bench(nlohmann::json request);

    /// Stop the run: the item in progress is abandoned and what was finished
    /// is kept, marked as stopped.
    void stop_bench();

    /// True while a run goes. A chat turn and a build are refused meanwhile,
    /// and nothing is loaded for one: the run has the cards to itself, and
    /// what it times is its model alone.
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
        bool                            answer_only = false;   ///< never a build; see submit

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
    /// "Load on demand" decides when the delegator is *freed* -- the moment it
    /// has routed, so the expert gets every byte -- not when it comes back.
    /// It comes back as soon as the work is over, so that the next prompt is
    /// routed the instant it is sent rather than after a load nobody asked to
    /// wait for.
    void ready_delegator();

    /// Put the table back after a prompt, however it ended: the chat's own
    /// Stop is cleared, and then tidy_models.
    void settle();

    /// With the delegator on demand: free the experts nobody is using and
    /// bring the delegator back. Called once a prompt or a build is over, so
    /// the window is free to take the next prompt while it happens -- the
    /// prompt simply waits its turn behind the load. Any thread.
    void tidy_models();

    /// Make sure the delegator is loaded, freeing an expert nobody is using
    /// first if there is no room for both. A no-op when it is already there,
    /// or none is set. Call with host_mutex_ held. `cancel` as load_router's.
    void ensure_router(const CancelCallback& cancel = {});

    /// Drop the delegator: between routes when "keep delegator loaded" is
    /// off, and in either mode for Eject, a changed runtime and changed
    /// loading settings. Call with host_mutex_ held.
    void release_router();

    /// release_router() for the two places that free the delegator on their
    /// own -- an expert about to load, a build's seat taken -- which must not
    /// do it in the middle of somebody else's route. Takes host_mutex_; call
    /// without it.
    void release_router_between_routes();
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
    // Routing, a chat turn's rounds and the cook and build loops are Python,
    // in a process of their own: see orchestra/link.hpp. They ask the core for
    // what only the core may do, and it answers here, on threads of the
    // link's own: see serve().

    /// Answer one of the orchestrator's requests. Throws with the reason when
    /// it cannot be answered, which goes back to the orchestrator as an error.
    ///
    /// Called on a thread of the link's own, one per request, so the chat's
    /// routing and every agent of a build are answered side by side. Whose a
    /// request is -- the chat's or the build's -- is in its "context", and it
    /// decides which Stop the request heeds.
    nlohmann::json serve(const std::string& method, const nlohmann::json& params);

    /// What the orchestrator needs to route: the prompt, the roster as the
    /// delegator is shown it, which seats have a model, and the policy, all
    /// as `config` has them.
    static nlohmann::json routing_request(const Config& config, const std::string& prompt,
                                          const std::optional<ExpertId>& pinned);

    /// A tool call as the orchestrator sends one back: the fields tools.parse
    /// handed it, kind by name.
    static tools::ToolCall call_from(const nlohmann::json& params);

    /// Have Crucible's Python, waiting for it while it is being downloaded.
    /// False, with `error` saying why, when it is not coming.
    bool wait_for_python(std::string& error, const CancelCallback& cancel = {});

    orchestra::Link orchestra_;

    // --- the cook, in engine_cook.cpp ---------------------------------------
    void do_cook(const std::string& id, const std::string& goal, int budget_seconds,
                 const std::filesystem::path& root,
                 std::vector<attach::Attachment> attachments,
                 std::optional<ExpertId> pinned, const std::string& kind);

    /// The turn that became the build `cook`, given what the build left as
    /// its reply, and the conversation's memory of it: see Turn::build.
    void settle_build_turn(const Cook& cook);

    /// The Stop a request from the orchestrator heeds: the build's for one of
    /// its agents, the chat's for a turn's routing.
    CancelCallback cancel_for(const nlohmann::json& params) const;

    /// Attachments as a message's text and pictures, sized to `model`:
    /// `share` of its context, less what `messages` already take.
    attach::Composed read_attachments(std::vector<attach::Attachment> attachments,
                                      const ChatModel& model,
                                      const std::vector<ChatMessage>& messages, double share);

    /// The workshop, pointed at `root`.
    ///
    /// One place, because chat and cook get the same tools -- asking a question
    /// about a file and setting a goal that changes it are the same permission,
    /// and a chat that could read but not write was a distinction nobody asked
    /// for. An empty root gives a disabled workshop, which is what an untrusted
    /// folder produces.
    tools::WorkshopSettings workshop_for(const std::filesystem::path& root) const;

    /// Publish the current journal to the UI. Called after every change.
    void publish_cook();

    /// One round: render the conversation, generate, and return what the expert
    /// said. Empty when it could not run at all.
    struct CookRound {
        std::string answer;
        std::string reasoning;
        long        ms = 0;

        /// Why there is no answer, when the model could not be asked at all.
        /// Only a provider produces one: a local model that is loaded answers.
        std::string error;

        /// What the journal should say besides: that another model answered
        /// in place of the one in the seat. See ChatResult::notes.
        std::vector<std::string> notes;
    };
    CookRound cook_round(ChatModel& model, const ModelParams& params,
                         const std::vector<ChatMessage>& messages);

    /// A model, held: the model to ask, and the lease that keeps it in
    /// memory for as long as this is alive. A provider's model needs no
    /// lease; a local one cannot be freed while one is held.
    struct HeldModel {
        ChatModel*     model = nullptr;
        Leases::Lease  lease;
    };

    /// One agent's seat in a build: who is in it, the model they are held
    /// on, and the pictures that go with their next round.
    ///
    /// A build has as many as it has agents working, each named by a handle
    /// the orchestrator passes back with every round -- "s3" -- so two agents
    /// asking at once are asking of two seats.
    struct AgentSeat {
        ExpertId     id;
        std::string  name;
        ModelParams  params;
        HeldModel    held;

        /// The pictures the build's directive came with, read for this seat's
        /// model -- sent with every round that carries the attachments.
        std::vector<ChatImage> images;

        /// Pictures the seat's last tool call produced -- a screenshot, a
        /// picture read -- for the round that follows it and no other: a
        /// screenshot sent again with every later round would cost the
        /// context more than it says.
        std::vector<ChatImage> tool_images;

        /// One round at a time per seat: the pictures above are the seat's
        /// own, and a seat is one agent.
        std::mutex   mutex;
    };
    std::shared_ptr<AgentSeat> seat_named(const nlohmann::json& params);

    std::mutex                                        seats_mutex_;
    std::map<std::string, std::shared_ptr<AgentSeat>> seats_;
    long                                              next_seat_ = 0;

    /// What the cook's goal came with, and the tools and search it works
    /// with. Set when a build starts and read by its agents; not changed
    /// while it runs.
    std::vector<attach::Attachment> cook_attachments_;
    tools::WorkshopSettings         cook_workshop_;
    tools::SearchSettings           cook_search_;

    /// The seats a build has made, waiting for the session. See take_made_seats.
    std::vector<MadeSeat>           made_seats_;   ///< under written_mutex_

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

    /// The project's history folder, for what a build records of its agents'
    /// work. Empty when no project is open.
    std::filesystem::path           project_dir_;

    /// The cook's half of serve(): the seat, the tools, the journal.
    nlohmann::json serve_cook(const std::string& method, const nlohmann::json& params);

    /// A benchmark run's half of serve(): the model under test, its replies,
    /// the datasets, the results. See engine_bench.cpp.
    nlohmann::json serve_bench(const std::string& method, const nlohmann::json& params);
    void do_bench(nlohmann::json request);

    /// Block until the user answers the question a cook is waiting on, or the
    /// cook is stopped. Returns the answer, or nothing if it was stopped.
    std::optional<std::string> await_cook_answer();

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

    /// One question to the person at a time, whoever is asking: the window
    /// shows one, and two agents asking at once would answer each other's.
    std::mutex asking_mutex_;

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
    /// Set once the worker has made host_: a build's thread can start before
    /// it has, and waits.
    std::atomic<bool>          host_ready_{false};

    /// The experts that are not on this machine. Nothing in it is resident in
    /// the sense `host_` means -- there are no weights -- so it sits beside
    /// the host rather than inside it: a seat is answered by one or the other,
    /// and asking a provider never evicts what is loaded here.
    ///
    /// Asked from the chat's thread and from each of a build's agents', and
    /// counting what every one of them costs. See remote_model.hpp.
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
    /// between requests -- a project cannot change while a turn or a cook is
    /// running, which is enforced above this by refusing to open one then.
    std::filesystem::path        project_root_;

    /// The cook in progress, on its own thread. The journal is the
    /// orchestrator's and is published through cook_mutex_, which also
    /// guards the log it is saved to.
    Cook                         cook_;
    std::unique_ptr<CookLog>     cook_log_;
    std::chrono::steady_clock::time_point cook_saved_at_{};   // see cook.publish
    std::mutex                   cook_mutex_;
    std::thread                  cook_thread_;
    std::atomic<bool>            cooking_{false};
    std::atomic<bool>            cook_stop_{false};
    std::atomic<bool>            cook_cancel_{false};

    /// The benchmark run, on its own thread like a cook, and the one seat it
    /// holds: the model being tested.
    std::thread                  bench_thread_;
    std::atomic<bool>            benching_{false};
    std::atomic<bool>            bench_stop_{false};
    std::mutex                   bench_mutex_;
    std::shared_ptr<AgentSeat>   bench_seat_;

    /// Ask the user about one edit and block until they answer: what a chat
    /// turn and a build's agent both ask. True means write it.
    ///
    /// False when the edit was declined, and also when the work was canceled
    /// or the engine is shutting down while parked here -- all three mean "do
    /// not write the file", which is the only question the caller has.
    /// `cancel` is whose Stop ends the wait: the chat's or the build's.
    bool ask_about_edit(const tools::ToolCall& call, const tools::WorkshopSettings& workshop,
                        const CancelCallback& cancel);

    /// The answer to an edit, handed across from the UI thread. Kept only
    /// while one is being asked: see approve_edit.
    std::mutex                   edit_mutex_;
    std::condition_variable      edit_answered_;
    std::optional<bool>          edit_approved_;
    bool                         edit_asking_ = false;

    /// The answer to a question a cook asked, handed across from the UI thread.
    std::mutex                   cook_answer_mutex_;
    std::condition_variable      cook_answered_;
    std::optional<std::string>   cook_answer_;

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
    /// running, and a cook is an hour of running.
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
        serve::Answer answer(const serve::Ask& ask, const serve::Sink& sink) override {
            return engine_.serve_request(ask, sink);
        }

    private:
        Engine& engine_;
    };

    /// A request from the server, answered. On the server's own thread.
    serve::Answer serve_request(const serve::Ask& ask, const serve::Sink& sink);

    /// The seat a model the server serves is held under, and what the
    /// right-hand panel says has it. Capitalized, which no seat's id is.
    static constexpr const char* kServerSeat = "Server";

    /// Listen as `config` says, or stop listening. From start() and
    /// apply_config(), on the thread that calls them.
    void configure_server(const Config& config);

    /// The file the server answers with, as `config` has it: the
    /// delegator's, or the model it names. Empty when there is none.
    static std::string served_file(const Config& config);

    /// What the server calls it: that file's name.
    static std::string served_id(const Config& config);

    /// How the served model loads: as the seat that has the same file does,
    /// so one copy in memory answers both, or as every seat starts from.
    /// `seat`, when given, is set to that seat, or to kServerSeat for none.
    static ModelParams served_params(const Config& config, ExpertId* seat = nullptr);

    /// Whoever a request waits behind while the models' lock is taken, in
    /// the words Settings shows: "the chat", "the build", "loading".
    std::string serve_blocker() const;

    /// One request, answered by `model` with `params` for what the request
    /// left out. `local` is the same model when it is one on this machine,
    /// which a text completion needs; null otherwise.
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

/// What a seat a build makes runs on. See made_seat_model.
struct SeatModel {
    std::string model;      ///< as a seat names one: a file in the models folder, or a provider's name for it
    std::string provider;   ///< empty for a model on this machine

    /// The model set for new agents and why it was passed over, when it was
    /// ("does not fit the cards here"); empty when it is the one.
    std::string passed_over;
    std::string why;
};

/// The model for a seat a build makes: the one Settings, Build sets for new
/// agents, unless `why_not` -- which says why a model cannot be loaded here,
/// or nothing when it can -- has a reason; then the model of the first seat
/// in `instead` that it has none for. Those are the build's own seats, the
/// one the task was routed to first, which are loading here already. No
/// model when none of them can be either. `config` has its models resolved.
SeatModel made_seat_model(const Config& config, const std::vector<ExpertId>& instead,
                          const std::function<std::string(const ModelParams&)>& why_not);

}  // namespace crucible
