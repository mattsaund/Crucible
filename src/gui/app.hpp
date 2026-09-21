// SPDX-License-Identifier: MIT
//
// The desktop face of Crucible.
//
// The same program as `crucible` in a terminal, and the word "same" is meant
// literally: this owns a Config, an AppState, an Engine, a SessionStore and a
// CookLog, exactly as the TUI does, and does not know anything about routing,
// cooking or models that the terminal does not. Everything below this file is
// shared. If the two ever disagree about what an expert is or how a cook
// finishes, that is a bug in one of the faces and not a difference of opinion.
//
// It follows that a feature added to the engine appears in both, and a feature
// added here is a drawing decision only.
//
// The one thing this has that the terminal program does not is a project
// picker. `crucible` is told where it is by being run there -- you cd, then you
// type it -- and a window has no cd, so it has to offer the list instead.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#include "crucible/config/config.hpp"
#include "crucible/config/trust.hpp"
#include "crucible/cook/journal.hpp"
#include "crucible/lab/hub.hpp"
#include "crucible/lab/recipe.hpp"
#include "crucible/util/display_scale.hpp"
#include "crucible/engine/engine.hpp"
#include "crucible/engine/state.hpp"
#include "crucible/llm/model_catalog.hpp"
#include "crucible/runtime/builder.hpp"
#include "crucible/runtime/registry.hpp"
#include "crucible/session/store.hpp"

struct GLFWwindow;

namespace crucible::gui {

class App {
public:
    /// `start` is the project to open, and `ask_trust` says the window has to
    /// put the folder-trust question up itself because there was no terminal to
    /// ask it on.
    App(Config config, std::vector<std::string> warnings,
        std::filesystem::path start, bool ask_trust);
    ~App();
    App(const App&)            = delete;
    App& operator=(const App&) = delete;

    /// Open the window and run until it is closed. Returns a process exit code.
    int run();

private:
    /// Which pane the main area is showing.
    ///
    /// No Experts pane: the sidebar section is called that, and managing them
    /// is a settings page like the rest of the configuration. Two things called
    /// Experts in one sidebar is a question the user should not have to answer.
    enum class View { Chat, Cook, Create, History, Settings };

    /// Which page of the settings. One list down the left and one page on the
    /// right, which is the shape every desktop application settles on because
    /// a single scrolling wall of switches cannot be navigated.
    enum class SettingsPage {
        General, Experts, Generation, Hardware, Runtimes, Tools, About
    };

    // --- frame ------------------------------------------------------------
    void draw();

    /// The bar across the top: the fold toggle, the mark, which project is
    /// open, the three views, and the way through to Settings.
    ///
    /// Takes no snapshot: everything on it is true of the window rather than of
    /// what the engine is doing this second. What the engine is doing moved to
    /// the side menu, over the models it is about.
    void draw_topbar();
    float topbar_height() const;

    /// Open or close the side menu from the button in the top bar, remembering
    /// the width it had so bringing it back does not reset it.
    void toggle_sidebar();

    void draw_sidebar(const Snapshot& snapshot);
    void draw_splitter();

    /// The delegator and the experts under it, and the line joining the two
    /// while a turn is flowing. Drawn together because the connector needs both
    /// ends before it can be drawn at all.
    void draw_model_tree(const Snapshot& snapshot);

    /// Go to a settings page, remembering where to come back to.
    void show_settings(SettingsPage page);

    /// The file an expert wants to change, as it is beside as it would be.
    ///
    /// Drawn under the transcript while the engine is parked waiting for an
    /// answer. Two panels, two buttons, and no third option: the file stays as
    /// it is, or it becomes the other one.
    void draw_pending_edit(const PendingEdit& edit);

    /// The one line a working view shows when there is nothing in it yet.
    ///
    /// Three states, one line each, no paragraph under any of them. Two are a
    /// thing to go and fix -- there is no backend to run a model on, or no
    /// model behind any expert -- and the third is the invitation. Which one
    /// you are in is a fact about the machine, and the two that are fixable are
    /// the button that takes you to where they are fixed.
    void draw_readiness();

    /// True when at least one backend module is installed, so a model can be
    /// loaded at all. Cached: it is a look at the filesystem, and the answer
    /// only changes when a runtime is built or removed from the settings page.
    bool any_runtime_ = false;

    /// The horizontal grab bar between the transcript and the composer, so the
    /// box you type in can be sized like the sidebar rather than only ever
    /// being as tall as what is already in it.
    void draw_composer_splitter();

    /// The sidebar's widths. Collapsed is a width below `sidebar_collapse_at`,
    /// not a separate mode; between there and `sidebar_min_width` it is drawn
    /// at the minimum, which is the gap that stops it flickering shut.
    bool  sidebar_collapsed() const;
    float sidebar_drawn_width() const;
    float sidebar_min_width() const;
    float sidebar_collapse_at() const;
    void draw_chat(const Snapshot& snapshot);
    void draw_cook(const Snapshot& snapshot);
    void draw_expert_list();

    /// The models a seat can be pointed at: the ones Crucible fine-tuned in the
    /// Create tab first, then the ones in the models directory. Returns what was
    /// picked this frame -- an empty string for "(none)" -- and nothing on every
    /// other frame.
    std::optional<std::string> draw_model_picker(const char* id, const std::string& current,
                                                 float width);
    void draw_history();

    /// The Create tab: assembling a model rather than running one.
    void draw_create(const Snapshot& snapshot);

    /// The hub search box and its results, for whichever step is asking.
    /// Returns what was picked this frame, and nothing on every other frame.
    std::optional<lab::Asset> draw_hub_picker(lab::hub::Kind kind, const char* placeholder);
    void draw_settings();

    /// The settings pages that are large enough to be worth their own file.
    /// Generation is every knob llama.cpp takes; Hardware is what to run on;
    /// Runtimes builds the backends that make hardware available at all.
    void draw_settings_generation();
    void draw_settings_hardware();
    void draw_settings_runtimes();

    /// Notice a runtime that has just finished building, once.
    ///
    /// The builder registers what it made with ggml before it reports Done, so
    /// the backend is live -- but every model already loaded picked its devices
    /// when it loaded and is still on them. Reloading is what makes a GPU
    /// installed from this screen take effect without restarting the window.
    ///
    /// Polled from the frame loop rather than from the Runtimes page, because a
    /// build takes minutes and the user is expected to go and watch something
    /// else while it runs.
    void take_runtime_activation();

    /// The bar under each of the two working views. Chat has a prompt box and
    /// nothing else; Cook has the goal, the budget and the buttons that start
    /// and stop it. They are separate because the two are separate actions and
    /// one bar could only ever do one of them.
    void draw_chat_composer(const Snapshot& snapshot);
    void draw_cook_composer(const Snapshot& snapshot);

    /// Tokens in and out this session, and how full the expert's context was on
    /// the last turn. Under the box on both the chat and the cook screen.
    void draw_usage_readout(const Snapshot& snapshot, float room, float column);

    /// How much room the composer needs this frame, so the pane above it can be
    /// given the rest. Computed rather than fixed: the boxes grow with what is
    /// typed into them.
    float composer_wanted_height(const Snapshot& snapshot);
    float composer_height(const Snapshot& snapshot);

    /// The height to give the text box inside the composer, or 0 to let it size
    /// itself to what is typed. Non-zero exactly when the user has dragged the
    /// composer to a height of their own, which the box then has to fill --
    /// otherwise dragging it taller would just add empty space under a box that
    /// stayed one line high.
    float composer_input_height() const;

    /// Measure the display the window is on and, when it differs from what the
    /// fonts and style were built for, rebuild both. `rebuild_texture` once the
    /// renderer exists and holds a font texture of its own.
    void apply_display_scale(bool rebuild_texture);

    /// Set while the composer splitter is being dragged, so the height it is
    /// being dragged to survives the frame that computes it.
    float composer_input_height_ = 0.0F;

    /// What the composer was actually drawn at last frame, which is where a
    /// drag starts from.
    float composer_drawn_height_ = 0.0F;

    void draw_new_expert_modal();
    void draw_browse_modal();
    void draw_trust_modal();

    /// What the folder browser is being opened to choose. The same browser
    /// serves both: picking a project and picking the models directory are the
    /// same question, and two copies of a directory list would drift apart.
    enum class BrowseFor { Project, ModelsDir };

    /// Open the folder browser at `start`, or at the obvious place for `what`
    /// when `start` is empty.
    void open_browse(BrowseFor what, const std::filesystem::path& start = {});

    /// One cook step: its verb, its summary, and the diff or output it expands
    /// into.
    void draw_cook_step(const CookStep& step, std::size_t index);

    // --- actions ----------------------------------------------------------
    void submit_prompt();
    void begin_cook();

    /// Stop whatever is running: a reply mid-flight, a cook, or a model that is
    /// still coming off the disk.
    ///
    /// The last of those is the one that was missing. A thirty-gigabyte expert
    /// is most of a minute of loading, and until the loader learned to be
    /// interrupted there was no way to end that minute -- which is a program
    /// that has frozen, from the only point of view that counts.
    void stop_work();

    /// Ask a turn's question again.
    ///
    /// The turn and everything after it goes, and the prompt is submitted
    /// fresh. Truncating rather than appending is what makes this a re-ask
    /// rather than a second ask: the replies that followed were answers in a
    /// conversation that is now going to be a different one, and the expert
    /// has to see the same context it saw the first time.
    void retry_turn(std::size_t index);

    /// Throw one turn away, question and answer together.
    void delete_turn(std::size_t index);

    /// Put the engine's conversation history back in step with the transcript.
    ///
    /// Called after anything that removes a turn. Without it the expert still
    /// remembers an exchange the user has deleted from the screen, which is the
    /// difference between deleting something and hiding it.
    void rebuild_history();
    void update_config(const std::function<void(Config&)>& change);
    void persist_session();
    void refresh_models();

    /// Take any routing examples the delegator wrote for itself and put them in
    /// the config, so a seat that has learned what it is for keeps that across
    /// restarts. An expert added with `/newexpert` starts with the blurb the
    /// user typed and earns its examples by being routed to.
    void absorb_written_examples();

    /// Post a line to the status strip. The last few only: this is a status
    /// channel, not a log.
    void say(std::string message);

    /// Point Crucible at another directory: new history, new cook journal, new
    /// workshop root. Refused while a cook is running, because the cook is
    /// about the directory it started in.
    ///
    /// Goes through the same folder-trust store the terminal program uses. A
    /// directory trusted in one face is trusted in the other.
    void open_project(const std::filesystem::path& root);

    Config                  config_;
    AppState                state_;
    std::unique_ptr<SessionStore> store_;
    std::unique_ptr<Engine> engine_;
    TrustStore              trust_;
    GLFWwindow*             window_ = nullptr;

    View         view_          = View::Chat;
    SettingsPage settings_page_ = SettingsPage::General;

    /// Where the gear came from, so pressing it again goes back there. Settings
    /// is a place you visit and leave, not a fourth tab you land in.
    View         before_settings_ = View::Chat;

    std::string prompt_;
    std::string cook_goal_;

    /// Width of the left column in pixels, dragged by the splitter. Per-session:
    /// it is how the window is arranged right now, not a preference worth
    /// writing to the config file. Below `sidebar_collapse_at()` the sidebar is
    /// closed, which is why there is no separate "is it open" flag.
    float sidebar_width_ = -1.0F;  ///< negative until the first frame sizes it

    /// The display scale the fonts and style were last built for.
    util::DisplayScale display_scale_{};

    // --- the lab ------------------------------------------------------------
    //
    // What the Create tab is assembling. Saved as it is filled in: gathering a
    // few gigabytes of training data is not something anyone finishes in one
    // sitting, and a recipe that only existed in memory would not survive it.
    lab::Recipe lab_recipe_;
    int         lab_step_ = 0;

    /// Every recipe on disk, so several models are in progress at once. The
    /// point of the tab is a roster of subject experts -- math, physics,
    /// programming -- and one at a time is not a roster.
    std::vector<lab::Recipe> lab_saved_;
    bool                     lab_saved_read_ = false;

    /// The hub search box, its answer, and what went wrong with it.
    std::string                 lab_query_;
    std::vector<lab::hub::Item> lab_results_;
    std::string                 lab_error_;
    std::string                 lab_local_path_;

    /// A search in flight. Hugging Face takes a second or two to answer and the
    /// window must not stop drawing while it does, so the worker fills this in
    /// and the next frame picks it up.
    struct LabSearch {
        std::mutex                  mutex;
        std::vector<lab::hub::Item> items;
        std::string                 error;
        bool                        done = false;
    };
    std::shared_ptr<LabSearch> lab_searching_;

    /// The width to reopen at. Set when the fold button closes the side menu,
    /// because closing it leaves `sidebar_width_` at zero and reopening to a
    /// hardcoded default would throw away a width the user chose.
    float sidebar_restore_ = 0.0F;

    /// Height the user has dragged the composer to, or 0 for "as tall as what
    /// is typed in it". Same idea as the sidebar width and kept for the same
    /// reason: an arrangement of this window, not a setting.
    float composer_height_ = 0.0F;

    /// Set when the transcript should jump to the bottom on the next frame.
    ///
    /// A flag rather than an unconditional scroll: a user reading back through
    /// an hour-old cook while a new one streams must not be yanked to the end
    /// every time a token arrives.
    bool follow_ = true;

    /// Which cook steps are expanded to show their diff or output. By index
    /// into the journal, which is stable for the life of a cook.
    std::vector<bool> expanded_;

    /// The new-expert dialog's two boxes, and what to say when it is refused.
    bool        expert_modal_open_ = false;
    std::string new_expert_name_;
    std::string new_expert_model_;   ///< what the new seat will run, or empty
    std::string new_expert_blurb_;
    std::string expert_error_;

    /// The folder browser: where it is looking, what it is choosing, and the
    /// name of a folder to create.
    bool                  browse_modal_open_ = false;
    BrowseFor             browse_for_        = BrowseFor::Project;
    std::filesystem::path browse_;
    std::string           browse_text_;
    std::string           project_error_;

    /// A directory waiting on the trust question, and the answer to it.
    std::optional<std::filesystem::path> pending_trust_;

    /// Set when the startup directory has not been trusted yet, so the question
    /// goes up on the first frame -- there was no terminal to ask it on before
    /// the window existed. Cleared once asked.
    bool ask_trust_on_open_ = false;

    /// The runtime manager's state. Scanned on first sight of the page rather
    /// than at startup: it touches the filesystem, and most sessions never open
    /// it. The builder outlives a page switch on purpose -- a CUDA build takes
    /// minutes, and clicking away from the page must not abandon it.
    RuntimeBuilder             runtime_builder_;
    std::vector<RuntimeStatus> runtimes_;
    std::string                runtime_error_;
    bool                       runtimes_scanned_ = false;

    /// One-shot latch for the above: Done stays Done until it is dismissed, and
    /// reloading every model on each of those frames would be a loop.
    bool                       runtime_activated_ = false;

    std::vector<ModelFile>   models_;   ///< the models directory, rescanned on demand

    /// What the lab has finished, offered beside them. Rescanned with the
    /// models directory, since both answer the same question.
    std::vector<lab::Made>   lab_made_;
    std::vector<std::string> notices_;
    std::size_t persisted_turns_ = 0;
};

}  // namespace crucible::gui
