// SPDX-License-Identifier: MIT
// The delegation loop, running on its own thread.
//
// Everything llama.cpp touches lives here. The UI hands the engine a prompt and
// gets told, via AppState plus a wake callback, how the delegation is going.
// The engine blocks for seconds at a time loading a 30B expert; keeping it off
// the UI thread is what lets the expert panel keep animating while that happens.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <filesystem>

#include "crucible/config/config.hpp"
#include "crucible/cook/journal.hpp"
#include "crucible/llm/model_host.hpp"
#include "crucible/routing/router.hpp"
#include "crucible/engine/state.hpp"
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
    void submit(std::string prompt, std::optional<ExpertId> pinned = std::nullopt);

    /// Ask the current generation to stop at the next token boundary.
    void cancel();

    /// Drop the resident expert, freeing its memory without exiting.
    void release_expert();

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
    /// Queued like any other request, and once it starts it holds the worker
    /// for its whole duration. Prompts submitted while it runs wait behind it,
    /// which is the truthful behavior: there is one engine and it is busy.
    void start_cook(std::string goal, int budget_seconds, std::filesystem::path root);

    /// Ask the running cook to wrap up.
    ///
    /// Not a kill: the cook stops taking new work, makes a finishing pass to
    /// leave the project in a working state, and writes what it did. That pass
    /// is the whole reason this is different from cancel().
    void stop_cook();

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

    /// Ask the delegator to write worked examples for a seat that has none.
    ///
    /// Two example questions per expert are worth seven points of routing
    /// accuracy on the benchmark (89% to 96%), and they are the one thing
    /// `/newexpert` cannot ask a person for: a description is something you can
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

    /// Replace the running configuration, as the settings screen does.
    ///
    /// Applied on the worker thread between requests, never mid-generation.
    /// A changed router model is reloaded; an expert whose model changed is
    /// evicted so the next prompt picks up the new file.
    void apply_config(Config config);

    /// A snapshot of the configuration currently in force.
    Config config() const;

    bool is_busy() const { return busy_.load(std::memory_order_relaxed); }

private:
    /// One unit of work for the engine thread. `kind` keeps config changes and
    /// expert releases on the same queue as prompts, so they are applied in
    /// order and never race with a generation in flight.
    enum class RequestKind { Prompt, ReleaseExpert, ReloadModels, ApplyConfig,
                             WriteExamples, Cook };

    struct Request {
        RequestKind             kind = RequestKind::Prompt;
        std::string             prompt;
        std::optional<ExpertId> pinned;
        Config                  config;
        ExpertId                expert;  ///< for WriteExamples

        // for Cook
        int                   budget_seconds = 0;
        std::filesystem::path root;
    };

    void run();
    void handle(const Request& request);
    void load_router();
    void load_router_if_resident();

    /// Make sure the delegator is loaded, freeing the expert first if there is
    /// no room for both. A no-op when it is already there.
    void ensure_router();

    /// Drop the delegator, wrapper first. Only called with "keep delegator
    /// loaded" off.
    void release_router();
    void do_apply_config(Config config);
    void do_write_examples(const ExpertId& id);

    // --- the cook loop, in engine_cook.cpp --------------------------------
    void do_cook(const std::string& goal, int budget_seconds,
                 const std::filesystem::path& root);

    /// The workshop, pointed at `root`.
    ///
    /// One place, because chat and cook get the same tools -- asking a question
    /// about a file and setting a goal that changes it are the same permission,
    /// and a chat that could read but not write was a distinction nobody asked
    /// for. An empty root gives a disabled workshop, which is what an untrusted
    /// folder produces.
    tools::WorkshopSettings workshop_for(const std::filesystem::path& root) const;

    /// Record a step, publish the journal and write it to disk.
    void note_step(CookStep step);

    /// Publish the current journal to the UI. Called after every change.
    void publish_cook();

    /// One round: render the conversation, generate, and return what the expert
    /// said. Empty when it could not run at all.
    struct CookRound {
        std::string answer;
        std::string reasoning;
        long        ms = 0;
    };
    CookRound cook_round(LoadedModel& model, const ModelParams& params,
                         const std::vector<ChatMessage>& messages);

    /// Who is in the seat for a cook, and the model behind them.
    ///
    /// A cook is not one expert any more. It starts with whoever the goal
    /// routes to, and any HANDOFF sends the next piece of work back through the
    /// delegator -- so a programming expert that has finished the code can say
    /// the next thing needed is documentation and have a writing expert put in
    /// its place.
    struct CookSeat {
        ExpertId     id;
        std::string  name;
        ModelParams  params;
        LoadedModel* model = nullptr;
        std::string  error;   ///< set when the model could not be loaded
    };

    /// Route `work` and make the winner resident, freeing whoever was there.
    ///
    /// Returns the seat that is actually loaded. On failure `model` is null and
    /// `error` says why. A no-op when the delegator picks whoever is already in
    /// the chair, which is the common case and must not cost a reload.
    CookSeat take_the_seat(const std::string& work, const CookSeat& current);

    /// Block until the user answers the question a cook is waiting on, or the
    /// cook is stopped. Returns the answer, or nothing if it was stopped.
    std::optional<std::string> await_cook_answer();

    /// Pick the expert that will actually answer. The router names a subject;
    /// this decides what to do when that subject has no model configured.
    RouteDecision resolve(const Request& request);

    Config                 config_;
    mutable std::mutex     config_mutex_;   ///< guards config_ against the UI thread
    AppState&              state_;
    std::function<void()>  wake_;

    std::unique_ptr<ModelHost> host_;
    std::unique_ptr<Router>    router_;

    /// The delegator's measured bias, kept across reloads of the same file so
    /// an on-demand delegator does not re-measure it every prompt. See
    /// ModelRouter::bias.
    std::vector<float>         router_bias_;
    std::string                router_bias_for_;

    std::vector<ChatMessage> history_;

    /// See take_written_examples. Guarded by its own mutex rather than by
    /// `mutex_`, which the worker holds while it waits for work.
    std::mutex written_mutex_;
    std::vector<std::pair<ExpertId, std::vector<std::string>>> written_examples_;

    /// The directory this session is about, and the one thing that decides
    /// whether an expert may touch the disk.
    ///
    /// Set from the UI thread by set_project and read by the worker, both only
    /// between requests -- a project cannot change while a turn or a cook is
    /// running, which is enforced above this by refusing to open one then.
    std::filesystem::path        project_root_;

    /// The cook in progress. Touched only by the worker thread, except for the
    /// two atomics below, which the UI thread sets.
    Cook                         cook_;
    std::unique_ptr<CookLog>     cook_log_;
    std::atomic<bool>            cooking_{false};
    std::atomic<bool>            cook_stop_{false};

    /// Ask the user about one edit and block until they answer.
    ///
    /// Returns false when the edit was declined, and also when the turn was
    /// canceled or the engine is shutting down while parked here -- all three
    /// mean "do not write the file", which is the only question the caller has.
    bool await_edit_approval(const tools::ToolCall& call,
                             const tools::WorkshopSettings& workshop);

    /// The answer to an edit, handed across from the UI thread.
    std::mutex                   edit_mutex_;
    std::condition_variable      edit_answered_;
    std::optional<bool>          edit_approved_;

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
};

}  // namespace crucible
