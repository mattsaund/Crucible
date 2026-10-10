// SPDX-License-Identifier: MIT
//
// The session.
//
// Everything Crucible is, apart from what draws it: a Config, an AppState, an
// Engine, a SessionStore, a TrustStore and the lab's trainer. It owns them and
// decides what may happen to them -- which project is open, what a change to
// the configuration means, whether a folder has been trusted.
//
// What draws it is window.cpp, and it reaches none of this directly. It puts
// an api::Surface in front of this object and goes through that, exactly as
// the Python orchestrator will. That is the whole reason the split is here:
// an interface that could reach in would become the only interface that could.
//
// So the class is an api::Host and almost nothing else is public. What the
// surface may ask of a session is the interface; how the session answers is
// below it.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "crucible/api/surface.hpp"
#include "crucible/app/self_source.hpp"
#include "crucible/app/setup.hpp"
#include "crucible/app/update.hpp"
#include "crucible/config/config.hpp"
#include "crucible/config/trust.hpp"
#include "crucible/engine/engine.hpp"
#include "crucible/engine/state.hpp"
#include "crucible/lab/recipe.hpp"
#include "crucible/lab/pyenv.hpp"
#include "crucible/lab/trainer.hpp"
#include "crucible/runtime/builder.hpp"
#include "crucible/session/store.hpp"
#include "crucible/util/workers.hpp"

namespace crucible::gui {

class App final : private api::Host {
public:
    /// `skip_trust` says not to ask about a folder before working in it, which
    /// is for a scripted run where there is nobody to answer.
    App(Config config, std::vector<std::string> warnings, bool skip_trust);
    ~App() override;
    App(const App&)            = delete;
    App& operator=(const App&) = delete;

    /// Open the window and run until it is closed. Returns a process exit code.
    ///
    /// The window is the platform's own webview on an embedded page, and it
    /// is the only one: see window.cpp.
    int run();

private:
    // --- what the surface may ask: api::Host -------------------------------
    //
    // Documented where they are declared, in api/surface.hpp. Each is a line
    // or two here; the work is in the members further down.

    Engine*                engine() override { return engine_.get(); }
    AppState*              state() override { return &state_; }
    lab::Trainer*          trainer() override { return &trainer_; }
    RuntimeBuilder*        runtime_builder() override { return &runtime_builder_; }
    lab::pyenv::Installer* trainer_installer() override { return &pyenv_installer_; }
    Setup*                 setup() override { return &setup_; }
    self::Rebuild*         rebuild() override { return &rebuild_; }
    void                   quit() override;

    Config      config() const override { return config_; }
    std::string apply_config(Config edited) override;

    std::filesystem::path project_root() const override;
    std::string           open_project(const std::filesystem::path& root) override;
    std::string           open_scratchpad() override;
    std::string           new_session() override;
    std::string           session_id() const override;
    std::string           session_name() const override;
    std::filesystem::path pending_trust() const override;
    void                  answer_trust(bool trusted) override;

    std::string open_session(const std::string& id) override;
    std::string delete_session(const std::string& id, const std::filesystem::path& root) override;
    bool        retry_turn(std::size_t index) override;
    bool        delete_turn(std::size_t index) override;

    std::string test_begin(const std::string& recipe_id) override;
    void        test_end() override;
    std::string keep(const std::string& recipe_id) override;

    void          say(std::string message) override;
    update::State update() const override { return update_; }
    void          wake() override;

    // --- the project ------------------------------------------------------

    /// The history folder beside the project. Empty until one is opened,
    /// which is a real state: the program opens on nothing rather than
    /// guessing a folder.
    std::filesystem::path project_dir() const;
    bool project_open() const { return store_ != nullptr; }

    // --- state the session owns -------------------------------------------

    /// Change the configuration: apply it, save it, and hand it to the engine.
    /// The one path, so a change made anywhere means the same thing.
    void update_config(const std::function<void(Config&)>& change);

    /// Rebuild what the expert can see from what is on screen. Called after
    /// anything that changes the transcript out from under it.
    void rebuild_history();

    // --- what a frame loop used to do --------------------------------------
    //
    // A page has no frame loop. These are the jobs the old window did once a
    // frame without anybody deciding it should: each looks to see whether
    // there is anything to do, and nearly every time there is not. The window
    // calls `housekeeping` before each snapshot it pushes.

    void housekeeping();

    /// Write the conversation to the project's history, if anything changed.
    void persist_session();

    /// Fold in the worked examples the delegator wrote for new seats.
    void absorb_written_examples();

    /// Keep the seats a build made for itself: into the config file, with
    /// examples asked for, like a seat added by hand.
    void absorb_made_seats();
    void absorb_starter_model();

    /// Ask for the conversation on screen to be named, once it has had an
    /// exchange and has no name; and file the names that have come back.
    void name_sessions();

    /// The conversations a name has been asked for, so each is asked once.
    std::vector<std::string> naming_asked_;

    /// Close the project: no folder, no history, nothing on screen. What a new
    /// chat starts from -- its first message opens a scratch folder of its
    /// own.
    void close_project();

    /// A training run that has finished, written back to the recipe it
    /// belongs to -- whichever view is showing, so a run that ends while you
    /// are in Chat is ready to test when you come back.
    void absorb_finished_run();

    /// A runtime that has finished building, put to use: the models are
    /// dropped so that the next prompt loads onto the hardware it unlocked.
    void absorb_finished_build();

    // --- trying a fine-tune before keeping it -----------------------------
    //
    // A model that has finished training is a file, not yet an expert. You
    // talk to it first, on a seat that exists only while you are talking to
    // it, and then decide. Keeping it is what puts it on the roster.

    //
    // The seat is handed to the engine and never written to the config: a
    // seat called "(testing)" surviving a crash is exactly the litter that
    // avoids. See test_begin, test_end and keep above.

    /// The seat a candidate is tried on. One id for every test rather than
    /// one per recipe, because only one can be open at a time.
    static constexpr const char* kTestSeat = "lab-test";

    /// Which recipe is seated, so that ending the test knows there is
    /// something to undo.
    std::string testing_;

    /// The last run and the last build that were acted on, so each is acted
    /// on once. Progress is read every time the page is redrawn; "it has
    /// finished" stays true for as long as nobody starts another.
    std::string run_absorbed_;
    bool        build_absorbed_ = false;

    // --- is there a newer Crucible ----------------------------------------
    //
    // Read from the cache at startup, which costs a file read and no network,
    // and refreshed at most once a day on a thread of its own.
    void begin_update_check();
    void collect_update_check();

    /// The frontier models' list prices, fetched on a worker when a provider
    /// is added and the list is missing or a day old. See llm/prices.hpp.
    void refresh_prices();

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
    /// The engine, the installers, the trainer and the update check all run
    /// on threads of their own, and some of them outlive the window. So this
    /// is set and cleared under a lock, and `wake` calls it under the same
    /// one: once it has been cleared, nothing is still inside it.
    void set_wake(std::function<void()> wake);

    std::mutex            wake_mutex_;
    std::function<void()> wake_;

    /// The folder waiting to be trusted, and whether to ask at all.
    std::optional<std::filesystem::path> pending_trust_;
    bool                                 skip_trust_ = false;

    /// Long-running installs the interface can start and watch. One of each,
    /// because each is one machine-wide thing: two CUDA builds at once would
    /// write the same files.
    RuntimeBuilder        runtime_builder_;
    lab::pyenv::Installer pyenv_installer_;
    lab::Trainer          trainer_;

    /// Fetches what the download did not carry, through the two above. After
    /// them, so it is gone before they are. See app/setup.hpp.
    Setup                 setup_{runtime_builder_, pyenv_installer_,
                                 [this] { wake(); },
                                 [this] { if (engine_) { engine_->reload_models(); } }};

    std::size_t persisted_turns_ = 0;

    /// A rebuild of Crucible's own source, when somebody asks for one. See
    /// app/self_source.hpp.
    self::Rebuild         rebuild_;

    /// The window while it is open, for quit(): a webview, held as void so
    /// this header does not need webview's. Set and cleared by run().
    std::atomic<void*>    view_{nullptr};

    /// Threads for the errands that are slow and touch nothing here: the
    /// lookups the surface marks as such, and the daily version check.
    ///
    /// Last, so that it is destroyed first -- an errand still running when
    /// the session goes away would otherwise finish into members that have
    /// already been torn down.
    util::Workers workers_;
};

}  // namespace crucible::gui
