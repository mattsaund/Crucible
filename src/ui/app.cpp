// SPDX-License-Identifier: MIT
//
// The application shell: layout, key handling, and the animation clock.
//
// This file owns the thread boundary. The engine runs elsewhere and pokes the
// screen through PostEvent, which is the only FTXUI call safe to make from off
// the UI thread; everything else here runs on the loop.
//
// Slash commands live in commands.cpp and the conversation in transcript.cpp,
// so what remains is the shell itself.
#include "crucible/ui/app.hpp"

#include <algorithm>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>

#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/terminal.hpp>

#include "crucible/config/paths.hpp"
#include "crucible/session/usage.hpp"
#include "crucible/ui/widgets/expert_panel.hpp"
#include "crucible/ui/settings/settings_view.hpp"
#include "crucible/ui/theme.hpp"
#include "crucible/ui/widgets/resource_meter.hpp"
#include "crucible/util/format.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace)

namespace crucible::ui {
namespace {

}  // namespace

App::App(Config config, const std::vector<std::string>& warnings)
    : config_(std::move(config)),
      sprite_(config_.ui.unicode),
      settings_(config_),
      // The runtime panel builds on a thread of its own and pokes the screen
      // when progress moves, exactly as the engine does.
      runtimes_([this] { screen_.PostEvent(Event::Custom); }),
      store_(Project::current()),
      screen_(ScreenInteractive::Fullscreen()) {
    show_experts_ = config_.ui.show_experts;

    for (const std::string& warning : warnings) {
        state_.add_notice(warning);
    }
    state_.configure_seats(config_);

    // What this project has spent before today, so the counter continues
    // rather than restarting at zero each time Crucible opens.
    state_.set_project_usage(store_.project_usage());

    // The engine runs on its own thread and pokes the screen when the state
    // changes. PostEvent is the only FTXUI call safe to make from off-thread.
    engine_ = std::make_unique<Engine>(config_, state_, [this] {
        screen_.PostEvent(Event::Custom);
    });
    // Cooks are journalled beside this project's conversations, and for the
    // same reason: Crucible is started inside a directory and is about that
    // directory.
    engine_->set_project(store_.project().root, store_.project().dir);

    InputOption option;
    option.multiline = false;  // FTXUI 7 defaults this on; we want Enter to send
    option.on_enter  = [this] { on_submit(); };
    // Bound so the completion can see the caret and move it after a Tab.
    option.cursor_position = &caret_;
    option.on_change = [this] { update_completion(); };
    input_ = Input(&input_text_, "ask Crucible anything, or /help", option);
}

App::~App() {
    // Every thread that can call back into this object has to be gone before
    // any member is destroyed. The screen is declared after the runtime panel
    // and so dies first, and a build thread still calling PostEvent to report
    // progress would be reaching into freed memory.
    stop_ticker();
    resources_.stop();
    runtimes_.shutdown();
    if (engine_) {
        engine_->stop();
    }
}

// ---------------------------------------------------------------------------
// Animation clock
// ---------------------------------------------------------------------------

void App::start_ticker() {
    ticking_.store(true, std::memory_order_relaxed);
    ticker_ = std::thread([this] {
        while (ticking_.load(std::memory_order_relaxed)) {
            // Animate briskly while Crucible is working, and slowly when he is
            // idle -- an idle crucible still flickers, but should not cost a redraw
            // ten times a second forever.
            const bool busy = state_.busy();
            const auto interval = std::chrono::milliseconds(
                busy ? std::max(30, config_.ui.animation_ms) : 400);
            std::this_thread::sleep_for(interval);

            if (!ticking_.load(std::memory_order_relaxed)) {
                break;
            }
            tick_.fetch_add(1, std::memory_order_relaxed);
            screen_.PostEvent(Event::Custom);
        }
    });
}

void App::stop_ticker() {
    if (!ticking_.exchange(false)) {
        return;
    }
    if (ticker_.joinable()) {
        ticker_.join();
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

App::TableView App::table_view() const {
    if (!show_experts_) {
        return TableView::Hidden;
    }
    // Leave room for a usable transcript: the ring only earns its space when
    // there is enough terminal left over to still read the conversation.
    const int rows = Terminal::Size().dimy;
    if (rows >= 34) { return TableView::Full; }
    if (rows >= 24) { return TableView::Compact; }
    return TableView::Strip;
}

Element App::render_status(const Snapshot& snapshot) const {
    std::string left = "● " + std::string(mood_label(snapshot.mood));
    if (snapshot.resident) {
        left += "  │  resident: " + expert_label(config_.roster, *snapshot.resident);
    } else {
        left += "  │  no expert loaded";
    }

    const std::string right = snapshot.busy
        ? "ctrl-c cancel  ·  /settings  ·  /help"
        : "ctrl-c quit  ·  /settings  ·  /resume  ·  /help";

    return hbox({
        text(left) | color(mood_color(snapshot.mood)),
        filler(),
        render_usage(snapshot),
        text("   " + right) | color(theme::kMeta) | dim,
    });
}

Element App::render_usage(const Snapshot& snapshot) const {
    // Session first, because it is the number that changes as you work. The
    // project total is the quieter one and sits behind a separator.
    const bool unicode = config_.ui.unicode;
    std::string text_out = usage_readout(snapshot.session_usage,
                                         snapshot.live_tokens_per_second, unicode);

    const TokenUsage& project = snapshot.project_usage;
    if (project.total_tokens() > snapshot.session_usage.total_tokens()) {
        text_out += unicode ? "   │  project " : "   |  project ";
        text_out += format_tokens(project.total_tokens()) + " tok";
    }

    return text(text_out) |
           color(snapshot.live_tokens_per_second > 0.0 ? Color(theme::kSeatActive)
                                                       : Color(theme::kMeta));
}

Element App::render() {
    // Panels stack: the runtime panel is opened from settings and drawn over
    // it, so leaving one returns to the other rather than to the transcript.
    if (runtimes_.active()) {
        return dbox({settings_.render(), runtimes_.render() | center});
    }
    if (gpu_order_.active()) {
        return dbox({settings_.render(), gpu_order_.render() | center});
    }
    if (models_.active()) {
        return dbox({settings_.render(), models_.render() | center});
    }
    if (in_settings_) {
        return settings_.render();
    }

    const Snapshot snapshot = state_.snapshot();
    const std::size_t tick  = tick_.load(std::memory_order_relaxed);

    Elements rows;

    // Overlaid on the expert panel rather than given a column: a panel beside the
    // ring would push Crucible off the center it is arranged around, and the
    // space to the right of the table is empty anyway.
    // Only where there is room beside the table. Narrower than that and the two
    // would be drawn on top of each other, which is worse than not having it.
    constexpr int kMeterNeeds = 104;
    const auto with_meter = [this](Element table) {
        if (screen_.dimx() < kMeterNeeds) {
            return table;
        }
        Element meter = resource_meter(resources_.snapshot());
        // Its own column, not a dbox overlay.
        //
        // Composited, the meter sat on top of whatever the centered panel
        // happened to reach underneath it, and on a wide terminal that is the
        // seat names: "Mathematics" rendered as "MatRTX 4070". Laid out beside
        // the table instead, the panel centers in what is left over and
        // the two cannot collide however many seats the roster grows.
        return hbox({
            std::move(table) | flex,
            vbox({std::move(meter), filler()}),
            text(" "),
        });
    };

    switch (table_view()) {
        case TableView::Full:
            rows.push_back(with_meter(expert_panel(snapshot, sprite_, tick, /*compact=*/false)));
            rows.push_back(separator());
            break;
        case TableView::Compact:
            rows.push_back(with_meter(expert_panel(snapshot, sprite_, tick, /*compact=*/true)));
            rows.push_back(separator());
            break;
        case TableView::Strip:
            rows.push_back(expert_strip(snapshot, tick));
            rows.push_back(separator());
            break;
        case TableView::Hidden:
            break;
    }

    rows.push_back(render_transcript(snapshot) | flex);
    rows.push_back(separator());
    rows.push_back(render_status(snapshot));
    // The command menu folds up out of the prompt, so it goes directly above
    // it and below everything else.
    if (Element menu = render_completion(); menu != nullptr) {
        rows.push_back(std::move(menu));
    }
    rows.push_back(render_prompt());

    Element screen = window(text(" Crucible " CRUCIBLE_VERSION " ") | bold | color(theme::kFlame),
                            vbox(std::move(rows)));

    if (sessions_.active()) {
        screen = dbox({std::move(screen), sessions_.render() | center});
    }
    if (expert_form_.active()) {
        screen = dbox({std::move(screen), expert_form_.render() | center});
    }
    return screen;
}

// ---------------------------------------------------------------------------
// Slash-command completion
//
// The matching itself is in completion.cpp, which knows nothing about
// terminals. What is here is the part that has to: where the menu is drawn,
// where the gray suffix lands relative to the cursor, and which keys mean
// "take it".
// ---------------------------------------------------------------------------

void App::update_completion() {
    completions_      = command_matches(input_text_, config_.roster);
    completion_index_ = 0;
    // Dismissal lasts until the line changes, so escape closes the menu for
    // the command being typed rather than for the rest of the session.
    completion_dismissed_ = false;
}

std::string App::completion_suffix() const {
    if (completions_.empty() || completion_dismissed_) {
        return {};
    }
    // Only with the caret at the end. Anywhere else the suggestion would be
    // drawn after text the user is no longer adding to, which would read as
    // characters that are already there.
    if (caret_ != static_cast<int>(input_text_.size())) {
        return {};
    }
    return command_completion(input_text_, completions_[completion_index_]);
}

void App::accept_completion() {
    const std::string suffix = completion_suffix();
    if (suffix.empty()) {
        return;
    }
    input_text_ += suffix;
    caret_ = static_cast<int>(input_text_.size());
    update_completion();
}

Element App::render_completion() const {
    if (completions_.empty() || completion_dismissed_) {
        return nullptr;
    }

    // Enough to choose from without burying the conversation. A "/" on its own
    // matches every command there is, and a menu twenty rows tall would push
    // the transcript off the screen every time the key was pressed.
    constexpr std::size_t kMaxRows = 8;
    const std::size_t shown = std::min(completions_.size(), kMaxRows);

    // Keep the highlighted row visible when the cursor has walked past the
    // window: scroll the list rather than the selection.
    std::size_t first = 0;
    if (completion_index_ >= shown) {
        first = completion_index_ - shown + 1;
    }

    std::size_t width = 0;
    for (const CommandInfo& command : completions_) {
        width = std::max(width, command.name.size());
    }

    Elements rows;
    for (std::size_t i = first; i < first + shown; ++i) {
        const CommandInfo& command = completions_[i];
        const bool selected = i == completion_index_;

        std::string name = "/" + command.name;
        name.resize(width + 2, ' ');

        Element row = hbox({
            text(selected ? " › " : "   ") | color(theme::kAccent),
            text(name) | (selected ? bold : nothing),
            text(command.summary) | color(meta_color(selected)),
        });
        rows.push_back(selected ? row | bgcolor(theme::kHighlight) : row);
    }

    if (completions_.size() > shown) {
        rows.push_back(text("   " + std::to_string(completions_.size() - shown) +
                            " more -- keep typing to narrow") |
                       color(theme::kMeta) | dim);
    }
    rows.push_back(text("   tab completes   ↑↓ choose   esc dismiss") |
                   color(theme::kMeta) | dim);

    return vbox(std::move(rows));
}

Element App::render_prompt() const {
    Element arrow = text(" › ") | color(theme::kAccent) | bold;

    const std::string suffix = completion_suffix();
    if (suffix.empty()) {
        return hbox({std::move(arrow), input_->Render() | flex});
    }

    // The suggestion is drawn as a second layer rather than as the next thing
    // in the row, and this is the reason: FTXUI's input ends with a blank cell
    // for the cursor to sit on, so anything placed after it starts one column
    // too far right and the line reads as two words with a space between them.
    // Clipping that cell off is no good either -- the input scrolls its
    // content to keep the cursor in view, so a narrower box eats the leading
    // "/" instead.
    //
    // Layering puts the first gray character *on* the cursor cell, which is
    // where it belongs: the cursor marks where typing would continue, and
    // where typing would continue is exactly the first suggested character.
    // The padding is emptyElement rather than spaces because dbox composites
    // by painting, and a space would paint over what is underneath it.
    constexpr int kArrowWidth = 3;  // " › "
    const int typed = static_cast<int>(string_width(input_text_));

    Element typed_row = hbox({
        std::move(arrow),
        input_->Render() | size(WIDTH, EQUAL, typed + 1),
        filler(),
    });
    Element suggestion = hbox({
        emptyElement() | size(WIDTH, EQUAL, kArrowWidth + typed),
        text(suffix) | color(theme::kMeta) | dim,
        filler(),
    });
    return dbox({std::move(typed_row), std::move(suggestion)});
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void App::absorb_written_examples() {
    if (!engine_) {
        return;
    }
    const std::vector<std::pair<ExpertId, std::vector<std::string>>> written =
        engine_->take_written_examples();
    if (written.empty()) {
        return;
    }

    // One save for however many arrived. update_config() writes the file and
    // hands the result back to the engine, and doing that once per expert would
    // mean a config write and a delegator rebuild each time.
    update_config([&written](Config& config) {
        for (const auto& [id, examples] : written) {
            const std::optional<std::size_t> seat = config.roster.find(id);
            if (!seat) {
                continue;  // ejected while the delegator was writing for it
            }
            Expert expert = config.roster.at(*seat);
            expert.examples = examples;
            config.roster.update(id, expert);
        }
    });

    for (const auto& [id, examples] : written) {
        say(expert_label(config_.roster, id) + ": the delegator wrote "
            + std::to_string(examples.size())
            + (examples.size() == 1 ? " example question" : " example questions")
            + " to route on");
    }
}

void App::open_settings() {
    // Start from what the engine is actually running, not from the copy this
    // object was constructed with, so the screen never shows stale values.
    settings_.set_config(engine_->config());
    settings_.set_status({});
    settings_.refresh();
    in_settings_ = true;
}

void App::update_config(const std::function<void(Config&)>& change) {
    // A slash command changing a setting goes through the settings screen's
    // copy, so the two can never disagree about what is configured -- and so it
    // is written and handed to the engine by the same path a typed change is.
    Config edited = settings_.config();
    change(edited);
    settings_.set_config(std::move(edited));
    save_settings(/*announce=*/false);
}

void App::scroll_by(int lines) {
    if (lines == 0) {
        return;
    }
    // Leaving the bottom is what stops following it; arriving back is what
    // resumes. Anything in between is a fixed position, so a reply streaming in
    // does not drag the view along while it is being read.
    const int overflow = std::max(0, content_height_ - viewport_height_);
    const int from     = follow_ ? 0 : scroll_;
    scroll_            = std::clamp(from - lines, 0, overflow);
    follow_            = scroll_ == 0;
}

void App::autosave() {
    // Settings write themselves as they are changed.
    //
    // Safe to do on every committed edit because that is what `dirty` means
    // here: a toggled checkbox, a picked model, a typed value that parsed. It
    // is not set per keystroke inside an editor, so this runs once per change
    // rather than once per key.
    if (settings_.dirty()) {
        save_settings(/*announce=*/false);
    }
}

void App::save_settings(bool announce) {
    Config edited = settings_.config();
    edited.resolve_models();

    if (!save_config(edited)) {
        // Always said, however the save was triggered: a settings screen that
        // silently fails to write is worse than one that never saved at all.
        settings_.set_status("could not write " + paths::config_file().string());
        return;
    }

    settings_.mark_saved();
    if (announce) {
        settings_.set_status("saved");
    }

    // The engine applies it between requests; the UI-side copy is updated here
    // so the expert panel and the mark pick up cosmetic changes immediately.
    config_ = edited;
    sprite_ = FlameSprite(config_.ui.unicode);
    show_experts_ = config_.ui.show_experts;
    state_.configure_seats(config_);
    engine_->apply_config(std::move(edited));
}

// ---------------------------------------------------------------------------
// Session history
// ---------------------------------------------------------------------------

void App::persist_session() {
    const Snapshot snapshot = state_.snapshot();

    // Only write when a turn has actually finished. The engine wakes the
    // screen for every token, and rewriting the file that often would be a
    // disk write per token for no gain.
    std::size_t finished = 0;
    for (const Turn& turn : snapshot.turns) {
        if (!turn.streaming && !turn.reply.empty()) {
            ++finished;
        }
    }
    if (finished == persisted_turns_) {
        return;
    }
    persisted_turns_ = finished;

    std::string error;
    if (!store_.save(snapshot.turns, snapshot.session_usage, error)) {
        say(error);
    }
}

void App::resume_session(const std::string& id) {
    std::vector<Turn> turns;
    TokenUsage usage;
    std::string error;
    if (!store_.load(id, turns, usage, error)) {
        say(error);
        return;
    }

    // Continue the stored conversation rather than forking it: further turns
    // append to the same file, which is what "resume" means.
    store_.adopt(id);
    state_.clear_turns();
    state_.clear_notices();

    // Hand the exchanges back to the engine too, or the expert would answer
    // the next question with no memory of what is on screen.
    engine_->reset_history();
    std::vector<ChatMessage> history;
    for (const Turn& turn : turns) {
        state_.restore_turn(turn);
        if (!turn.failed && !turn.canceled && !turn.reply.empty()) {
            history.push_back({"user", turn.prompt});
            history.push_back({"assistant", turn.reply});
        }
    }
    engine_->restore_history(std::move(history));

    persisted_turns_ = turns.size();
    follow_ = true;
    say("resumed " + id + " -- " + std::to_string(turns.size()) +
        (turns.size() == 1 ? " turn restored" : " turns restored"));
}

void App::on_submit() {
    const std::string text = format::trim(input_text_);
    input_text_.clear();
    caret_ = 0;
    completions_.clear();
    completion_dismissed_ = false;
    if (text.empty()) {
        return;
    }

    if (handle_command(text)) {
        return;
    }

    // An edit parked on approval takes the next thing typed as the answer, for
    // the same reason a cook's question does: the screen is showing two files
    // and asking which one you want, and nothing else typed under that would be
    // a reasonable reading. Anything but a clear yes leaves the file alone,
    // which is the safe way round for a question about overwriting something.
    if (state_.snapshot().pending_edit) {
        const bool yes = text == "y" || text == "Y" || text == "yes" || text == "apply";
        engine_->approve_edit(yes);
        say(yes ? "applied" : "left the file alone");
        follow_ = true;
        return;
    }

    // A cook parked on a question takes the next thing typed as the answer.
    // Anything else would be strange: the screen is showing a question, and the
    // only reasonable reading of a line typed under it is that it answers it.
    if (const std::shared_ptr<const Cook> cook = state_.cook();
        cook && cook->state == CookState::Asking) {
        engine_->answer_cook(text);
        follow_ = true;
        return;
    }

    if (engine_->cooking()) {
        // Queued behind the cook rather than refused. There is one engine and
        // it is busy for the next hour; saying so beats silently waiting.
        say("still cooking -- this will be answered when it finishes, or /stop it");
    }

    state_.clear_notices();
    engine_->submit(text);
    follow_ = true;
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

int App::run() {
    // FTXUI handles Ctrl-C itself by default *even when a component catches the
    // event*, and does so by re-raising SIGINT -- which would kill Crucible the
    // moment the user tried to interrupt a long answer. Taking ownership of the
    // key is what makes "Ctrl-C cancels, Ctrl-C again quits" possible.
    screen_.ForceHandleCtrlC(false);

    engine_->start();
    start_ticker();
    // Sampled on a thread of its own: nvidia-smi costs about sixty
    // milliseconds, which is most of a frame.
    resources_.start([this] { screen_.PostEvent(Event::Custom); });

    Component root = Renderer(input_, [this] { return render(); });

    root = CatchEvent(root, [this](const Event& event) {
        // The engine wakes the screen with a Custom event after every change.
        // A finished turn is the moment the conversation is worth writing, and
        // persist_session() returns immediately when nothing has finished.
        if (event == Event::Custom && !state_.busy()) {
            persist_session();
            absorb_written_examples();
        }

        // A runtime that just finished building is already registered with
        // ggml, but the models loaded before it are still on the devices they
        // chose at load time. Reloading them is what makes a GPU backend
        // installed from settings take effect without a restart.
        if (runtimes_.take_activation() && engine_) {
            engine_->reload_models();
        }

        // Modals are checked outermost-first, so the topmost one gets the key.

        // The runtime panel sits above settings.
        if (runtimes_.active()) {
            if (event == Event::CtrlC) {
                runtimes_.close();
                return true;
            }
            if (runtimes_.handle(event) == RuntimeAction::Close) {
                runtimes_.close();
                // Installing or removing a runtime changes what the hardware
                // settings can do -- a machine with one GPU cannot divide a
                // model between cards, and a machine with none cannot keep the
                // work off the processor. The rows work that out when they are
                // built, so they are rebuilt here rather than left describing
                // the hardware as it was when the screen opened.
                settings_.refresh();
            }
            return true;
        }

        // As does the GPU priority panel.
        if (gpu_order_.active()) {
            if (event == Event::CtrlC) {
                gpu_order_.close();
                return true;
            }
            switch (gpu_order_.handle(event)) {
                case GpuOrderAction::Apply:
                    // The panel owns the arrangement; the config only learns
                    // about it when something actually moved.
                    settings_.set_gpu_priority(gpu_order_.order());
                    autosave();
                    break;
                case GpuOrderAction::Close:
                    gpu_order_.close();
                    // Only when it has something to say: opening the panel and
                    // pressing escape should not wipe the settings status line.
                    if (!gpu_order_.status().empty()) {
                        settings_.set_status(gpu_order_.status());
                    }
                    break;
                case GpuOrderAction::None:
                    break;
            }
            return true;
        }

        // And the model manager.
        if (models_.active()) {
            if (event == Event::CtrlC) {
                models_.close();
                return true;
            }
            switch (models_.handle(event)) {
                case ModelManagerAction::Deleted:
                    // A deleted file empties whatever seat named it. Saved
                    // straight away rather than left dirty: the file is
                    // already gone, so a config still pointing at it is wrong
                    // on disk from this moment whatever the user does next.
                    settings_.forget_models(models_.take_removed());
                    save_settings();
                    settings_.set_status(models_.status());
                    break;
                case ModelManagerAction::Close:
                    models_.close();
                    if (!models_.status().empty()) {
                        settings_.set_status(models_.status());
                    }
                    break;
                case ModelManagerAction::None:
                    break;
            }
            return true;
        }

        // The new-expert form takes the keyboard while it is up: it is two
        // text boxes, so every printable key belongs to it.
        if (expert_form_.active()) {
            if (const std::optional<ExpertForm::Result> filled = expert_form_.handle(event)) {
                if (commit_new_expert(*filled)) {
                    expert_form_.close();
                }
            }
            return true;
        }

        if (sessions_.active()) {
            switch (sessions_.handle(event)) {
                case SessionPickerAction::Resume: {
                    const std::string chosen = sessions_.chosen();
                    sessions_.close();
                    resume_session(chosen);
                    return true;
                }
                case SessionPickerAction::Delete: {
                    std::string error;
                    if (!store_.remove(sessions_.chosen(), error)) {
                        say(error);
                    }
                    sessions_.refresh(store_);
                    return true;
                }
                case SessionPickerAction::Close:
                case SessionPickerAction::None:
                    break;
            }
            return true;
        }

        // The settings screen takes the keyboard while it is open, apart from
        // Ctrl-C, so there is always a way out.
        if (in_settings_) {
            if (event == Event::CtrlC) {
                in_settings_ = false;
                return true;
            }
            bool consumed = false;
            const SettingsAction action = settings_.handle(event, consumed);
            autosave();
            switch (action) {
                case SettingsAction::Close:
                    in_settings_ = false;
                    return true;
                case SettingsAction::Apply:
                    save_settings();
                    return true;
                case SettingsAction::OpenRuntimes:
                    runtimes_.open();
                    return true;
                case SettingsAction::OpenGpuOrder:
                    gpu_order_.open(settings_.config().gpu.priority);
                    return true;
                case SettingsAction::OpenModels:
                    models_.open(settings_.config().resolved_models_dir(),
                                 settings_.models_in_use());
                    return true;
                case SettingsAction::None:
                    break;
            }
            return consumed;
        }

        // The slash-command menu. Checked before the input component sees the
        // key, because Tab and the arrows would otherwise be swallowed by it.
        if (!completions_.empty() && !completion_dismissed_) {
            if (event == Event::Tab) {
                accept_completion();
                return true;
            }
            if (event == Event::ArrowDown) {
                completion_index_ = (completion_index_ + 1) % completions_.size();
                return true;
            }
            if (event == Event::ArrowUp) {
                completion_index_ =
                    (completion_index_ + completions_.size() - 1) % completions_.size();
                return true;
            }
            if (event == Event::Escape) {
                completion_dismissed_ = true;
                return true;
            }
        }

        if (event == Event::CtrlC) {
            // While Crucible is working, Ctrl-C stops the work rather than the
            // program -- the same contract every other REPL offers.
            if (state_.busy()) {
                engine_->cancel();
                return true;
            }
            should_exit_ = true;
            screen_.Exit();
            return true;
        }

        if (event == Event::CtrlT) {
            show_experts_ = !show_experts_;
            return true;
        }

        // --- scrolling the transcript ---------------------------------------
        if (event == Event::PageUp) {
            scroll_by(-std::max(1, viewport_height_ - 2));
            return true;
        }
        if (event == Event::PageDown) {
            scroll_by(std::max(1, viewport_height_ - 2));
            return true;
        }
        // The arrows, when nothing else wants them. The input is one line, so
        // up and down mean nothing to it, and the completion menu was offered
        // them first above.
        if (event == Event::ArrowUp) {
            scroll_by(-1);
            return true;
        }
        if (event == Event::ArrowDown) {
            scroll_by(1);
            return true;
        }
        if (event.is_mouse() && Event(event).mouse().button == Mouse::WheelUp) {
            scroll_by(-3);
            return true;
        }
        if (event.is_mouse() && Event(event).mouse().button == Mouse::WheelDown) {
            scroll_by(3);
            return true;
        }

        return false;
    });

    screen_.Loop(root);

    stop_ticker();
    resources_.stop();
    // Join the worker before any member is destroyed: its wake callback holds
    // a reference to the screen.
    engine_->stop();
    return 0;
}

}  // namespace crucible::ui
