// SPDX-License-Identifier: MIT
//
// The session.
//
// Everything Crucible is, apart from what draws it: a Config, an AppState, an
// Engine, a SessionStore, a TrustStore and the lab's trainer. It owns them and
// decides what may happen to them -- which project is open, what a change to
// the configuration means, whether a folder has been trusted.
//
// What draws it is webui.cpp, and it reaches none of this directly. It builds
// an api::Surface over these members and goes through that, exactly as the
// Python orchestrator will. That is the whole reason the split is here: an
// interface that could reach in would become the only interface that could.
//
// This was an ImGui window and a dozen panel files. They are gone -- the
// history has them -- and what they used to call back into is what is left.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "crucible/app/update.hpp"
#include "crucible/config/config.hpp"
#include "crucible/config/trust.hpp"
#include "crucible/engine/engine.hpp"
#include "crucible/engine/state.hpp"
#include "crucible/lab/recipe.hpp"
#include "crucible/lab/pyenv.hpp"
#include "crucible/lab/trainer.hpp"
#include "crucible/llm/model_catalog.hpp"
#include "crucible/runtime/builder.hpp"
#include "crucible/session/store.hpp"

namespace crucible::gui {

class App {
public:
    /// `skip_trust` says not to ask about a folder before working in it, which
    /// is for a scripted run where there is nobody to answer.
    App(Config config, std::vector<std::string> warnings, bool skip_trust);
    ~App();
    App(const App&)            = delete;
    App& operator=(const App&) = delete;

    /// Open the window and run until it is closed. Returns a process exit code.
    ///
    /// The window is the platform's own webview on an embedded page, and it
    /// is the only one: see webui.cpp.
    int run_web();

private:
    // --- the project ------------------------------------------------------

    /// The trusted directory being worked in, and the history folder beside
    /// it. Both empty until a project is opened, which is a real state: the
    /// program opens on nothing rather than guessing a folder.
    std::filesystem::path project_root() const;
    std::filesystem::path project_dir() const;
    bool project_open() const { return store_ != nullptr; }

    /// Open `root`, or -- when it has not been trusted yet -- put the question
    /// up by setting `pending_trust_` and open nothing.
    void open_project(const std::filesystem::path& root);

    /// Reopen a stored conversation: back onto the screen and back into the
    /// expert's context, appending to it rather than forking a new session.
    /// Returns a reason it could not, or an empty string.
    std::string resume_session(const std::string& id);

    // --- state the session owns -------------------------------------------

    /// Change the configuration: apply it, save it, and hand it to the engine.
    /// The one path, so a change made anywhere means the same thing.
    void update_config(const std::function<void(Config&)>& change);

    /// A line for the interface to show. A status channel, not a log.
    void say(std::string message);

    /// Rescan the models directory and what the lab has finished. Both answer
    /// the same question -- what can a seat be pointed at -- so they are asked
    /// together.
    void refresh_models();

    /// Write the conversation to the project's history, if anything changed.
    void persist_session();

    /// Fold in the worked examples the delegator wrote for new seats.
    void absorb_written_examples();

    /// Rebuild what the expert can see from what is on screen. Called after
    /// anything that changes the transcript out from under it.
    void rebuild_history();

    void retry_turn(std::size_t index);
    void delete_turn(std::size_t index);

    // --- trying a fine-tune before keeping it -----------------------------
    //
    // A model that has finished training is a file, not yet an expert. You
    // talk to it first, on a seat that exists only while you are talking to
    // it, and then decide. Keeping it is what puts it on the roster.

    /// Seat `recipe_id`'s trained file so it can be asked something. Returns
    /// a reason it could not, or an empty string.
    ///
    /// The seat is handed to the engine and never written to the config: a
    /// seat called "(testing)" surviving a crash is exactly the litter this
    /// avoids.
    std::string begin_test(const std::string& recipe_id);

    /// Take that seat away again. Safe to call when there is none.
    void end_test();

    /// Keep it: the recipe becomes finished and the model becomes an expert
    /// the delegator can route to. Returns a reason it could not, or empty.
    std::string keep_tested(const std::string& recipe_id);

    /// The seat a candidate is tried on. One id for every test rather than
    /// one per recipe, because only one can be open at a time.
    static constexpr const char* kTestSeat = "lab-test";

    /// Which recipe is seated, so the interface can be told and so closing
    /// the window knows there is something to undo.
    std::string testing_;

    // --- is there a newer Crucible ----------------------------------------
    //
    // Read from the cache at startup, which costs a file read and no network,
    // and refreshed at most once a day on a thread of its own.
    void begin_update_check();
    void collect_update_check();

    struct UpdateCheck {
        std::mutex    mutex;
        update::State state;
        bool          done = false;
    };
    std::shared_ptr<UpdateCheck> update_checking_;
    update::State                update_;

    bool update_available() const { return update::newer_than_this(update_); }

    // --- what it all is ----------------------------------------------------

    Config   config_;
    AppState state_;

    std::unique_ptr<SessionStore> store_;   ///< null until a project is opened
    std::unique_ptr<Engine>       engine_;
    TrustStore                    trust_;

    /// What to poke when something changed and the screen should be redrawn.
    ///
    /// The engine and the update check both run on their own threads and
    /// whatever is drawing may be parked waiting for input. Which nudge that
    /// is depends on who is drawing, so they are given this rather than the
    /// webview's dispatch queue directly.
    std::function<void()> wake_;

    /// The folder waiting to be trusted, and whether to ask at all.
    std::optional<std::filesystem::path> pending_trust_;
    bool                                 skip_trust_ = false;
    std::string                          project_error_;

    /// Long-running installs the interface can start and watch. One of each,
    /// because each is one machine-wide thing: two CUDA builds at once would
    /// write the same files.
    RuntimeBuilder        runtime_builder_;
    lab::pyenv::Installer pyenv_installer_;
    lab::Trainer          trainer_;

    std::vector<ModelFile>   models_;   ///< the models directory, rescanned on demand
    std::vector<lab::Made>   lab_made_; ///< what the lab has finished
    std::vector<std::string> notices_;
    std::size_t              persisted_turns_ = 0;
};

}  // namespace crucible::gui
