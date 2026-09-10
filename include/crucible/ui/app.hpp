// SPDX-License-Identifier: MIT
// The terminal application: layout, input handling, slash commands, and the
// animation clock that keeps Crucible moving while an expert loads.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "crucible/config/config.hpp"
#include "crucible/engine/engine.hpp"
#include "crucible/engine/state.hpp"
#include "crucible/session/store.hpp"
#include "crucible/routing/completion.hpp"
#include "crucible/ui/session_picker.hpp"
#include "crucible/ui/widgets/flame_sprite.hpp"
#include "crucible/ui/widgets/expert_form.hpp"
#include "crucible/ui/settings/runtime_view.hpp"
#include "crucible/ui/settings/gpu_order_view.hpp"
#include "crucible/ui/settings/model_manager_view.hpp"
#include "crucible/ui/settings/settings_view.hpp"
#include "crucible/util/resources.hpp"

namespace crucible::ui {

class App {
public:
    App(Config config, const std::vector<std::string>& warnings);
    ~App();
    App(const App&)            = delete;
    App& operator=(const App&) = delete;

    /// Run the TUI until the user quits. Returns a process exit code.
    int run();

private:
    /// How much of the expert panel the terminal has room for.
    enum class TableView { Full, Compact, Strip, Hidden };

    ftxui::Element render();
    void open_settings();
    /// Write the settings screen's config and hand it to the engine.
    /// `announce` puts "saved" on the status line, which is wanted for an
    /// explicit ctrl-s and only noise for an automatic one.
    void save_settings(bool announce = true);

    /// Save if the settings screen has an uncommitted change. Called after
    /// every key the settings screens see.
    void autosave();

    /// Change one thing in the configuration, then write and apply it. The way
    /// a slash command edits a setting.
    void update_config(const std::function<void(Config&)>& change);

    /// Write the conversation to the project's history. Called after every
    /// completed turn, so a crash costs at most the turn in flight.
    void persist_session();
    /// Replace the transcript with a stored conversation and continue it.
    void resume_session(const std::string& id);
    ftxui::Element render_transcript(const Snapshot& snapshot) const;
    ftxui::Element render_turn(const Turn& turn) const;
    ftxui::Element render_welcome() const;
    /// The live cook: its goal, its clock, and the last few things it did.
    ftxui::Element render_cook(const Cook& cook) const;
    ftxui::Element render_status(const Snapshot& snapshot) const;
    /// The token counters: session totals, project totals, and the live rate.
    ftxui::Element render_usage(const Snapshot& snapshot) const;

    // --- slash-command completion ------------------------------------------

    /// Re-match the menu against what is in the input box. Called after every
    /// keystroke the input consumed.
    void update_completion();
    /// The characters Tab would add, or empty when there is nothing to add.
    std::string completion_suffix() const;
    /// Accept the highlighted suggestion.
    void accept_completion();
    /// The menu that folds up above the prompt, or an empty element.
    ftxui::Element render_completion() const;
    /// The prompt row, with the gray suggestion trailing the cursor.
    ftxui::Element render_prompt() const;

    void on_submit();
    /// Returns true if the text was a slash command and has been dealt with.
    bool handle_command(const std::string& text);
    void say(std::string message);

    /// Fold any examples the delegator has written into the config and save
    /// them. Called when the screen wakes; returns at once when there are none.
    void absorb_written_examples();

    /// Take the filled-in `/newexpert` form and put a seat on the roster.
    ///
    /// Returns false with the form left open and an error on it when the name
    /// collides with a seat that already exists -- which is fixed by editing
    /// the name, not by typing the description again.
    bool commit_new_expert(const ExpertForm::Result& form);

    TableView table_view() const;
    void start_ticker();
    void stop_ticker();

    Config                  config_;
    AppState                state_;
    FlameSprite             sprite_;
    SettingsView            settings_;
    RuntimeView             runtimes_;
    GpuOrderView            gpu_order_;
    ModelManagerView        models_;
    SessionPicker           sessions_;
    ExpertForm              expert_form_;
    SessionStore            store_;
    bool                    in_settings_ = false;
    std::unique_ptr<Engine> engine_;

    /// How many turns had finished when the session was last written, so a
    /// redraw does not rewrite the file on every frame.
    std::size_t             persisted_turns_ = 0;

    ftxui::ScreenInteractive screen_;
    ftxui::Component         input_;
    std::string              input_text_;
    /// Bound into the Input component, so the completion knows where the caret
    /// is -- a suggestion trailing a cursor that is not at the end of the line
    /// would be drawn in the wrong place and mean the wrong thing. Written
    /// back to when Tab accepts one.
    int                      caret_ = 0;

    /// What "/…" could still become. Empty whenever the menu is closed.
    std::vector<CommandInfo> completions_;
    std::size_t              completion_index_ = 0;
    /// Escape closes the menu without clearing the input, and it stays closed
    /// until the text changes again.
    bool                     completion_dismissed_ = false;

    std::thread       ticker_;
    std::atomic<bool> ticking_{false};
    std::atomic<std::size_t> tick_{0};

    /// Read on its own thread and drawn in the corner of the expert panel.
    util::ResourceMonitor resources_;

    bool show_experts_ = true;

    /// Pin the transcript to its newest line. Any scroll up releases it; coming
    /// back to the bottom, or sending a prompt, takes it again -- so a reply
    /// arriving while you are reading history does not yank you away from it.
    bool follow_          = true;

    /// Which answer is under the cursor while an edit is waiting: 0 applies it,
    /// 1 leaves the file alone. Arrow keys move it, Enter takes it.
    ///
    /// Starts on "apply" because that is the answer nine times in ten, and a
    /// question whose common answer needs a keystroke to reach is a question
    /// that gets in the way. Escape and Ctrl-C both mean no, so the careful
    /// answer is never more than one key either.
    int edit_choice_ = 0;

    /// Lines scrolled up from the bottom, when not following.
    int  scroll_          = 0;

    /// Measured during layout and read on the next frame. See widgets/scroll.hpp.
    mutable int content_height_  = 0;
    mutable int viewport_height_ = 0;

    bool should_exit_     = false;

    /// Move the transcript by `lines` (negative scrolls up towards the start).
    void scroll_by(int lines);
};

}  // namespace crucible::ui
