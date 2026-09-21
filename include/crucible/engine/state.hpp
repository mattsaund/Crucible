// SPDX-License-Identifier: MIT
// The one piece of memory the engine thread and the UI thread share.
//
// The engine never touches ImGui and the UI never touches llama.cpp. They meet
// only here: the engine mutates this state under a lock and pokes the screen,
// and the renderer takes a Snapshot under the same lock. Nothing else crosses.
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "crucible/config/config.hpp"
#include "crucible/cook/journal.hpp"
#include "crucible/llm/model_host.hpp"
#include "crucible/routing/router.hpp"
#include "crucible/routing/expert.hpp"
#include "crucible/session/usage.hpp"

namespace crucible {

/// What Crucible himself is doing, which is what the sprite animates.
enum class Mood {
    Idle,
    Routing,   ///< the delegator is reading the prompt
    Loading,   ///< an expert is being swapped in
    Thinking,  ///< the expert is ingesting the prompt, no output yet
    Talking,   ///< tokens are streaming
    Error,
};

std::string_view mood_label(Mood mood);

/// One expert, as drawn.
enum class SeatPhase {
    Unconfigured,  ///< no GGUF assigned to this subject; drawn hollow
    Missing,       ///< a model is assigned but the file is not there
    Dormant,       ///< assigned, present on disk, not in memory
    Loading,       ///< being read in right now
    Active,        ///< resident and answering
};

struct SeatState {
    SeatPhase phase    = SeatPhase::Unconfigured;
    float     progress = 0.0F;  ///< 0..1 while Loading
};

/// Something a turn did to the world, and what came back.
struct TurnAction {
    /// One line, as it reads in the transcript: "wrote src/calc.py  +12 -3".
    std::string summary;

    /// What it produced and is worth keeping: the diff of a write, the output
    /// of a command. Empty for the ones that produce nothing worth a block --
    /// a listing, a search that only wants its own summary line.
    std::string body;

    /// What `body` should be colored as: a file name for a write, a fence
    /// language otherwise. Empty lets the renderer work it out from the text.
    std::string language;
};

/// One exchange. Kept as a unit so the transcript can show which expert
/// answered each turn, which is most of the point of the panel.
struct Turn {
    std::string            prompt;
    std::string            reply;

    /// The working a reasoning model does on the way to `reply`.
    ///
    /// Kept apart from the reply for two reasons. It is not the answer, and
    /// showing it as one is what made gpt-oss look broken. And it must not go
    /// back into the model's context next turn -- the format's own guidance is
    /// that previous reasoning is dropped, and feeding it back teaches the
    /// model that thinking aloud is part of the transcript.
    std::string            reasoning;

    /// What this turn did besides talk: a page it looked up, a file it rewrote,
    /// a command it ran -- and what that produced.
    ///
    /// Shown above the reply, because these are the parts of a turn that left
    /// the machine or changed something on it, and a reader has to be able to
    /// see them whether or not the answer mentions them. A model that edits a
    /// file and then writes a summary of having edited a different one is not
    /// rare, and this is the record that catches it.
    ///
    /// `body` is the reason this is a struct rather than a line of text. The
    /// diff a write produced and the output a command printed used to be
    /// thrown away the moment the next round started -- the tool call was wiped
    /// off the transcript as "a request, not an answer" and everything it made
    /// went with it. Scrolling back through an hour of work found the summaries
    /// and none of the code. It is kept now, for the life of the turn.
    std::vector<TurnAction> actions;
    std::optional<RouteDecision> route;
    bool                   streaming = false;
    bool                   canceled = false;
    bool                   failed    = false;
    double                 tokens_per_second = 0.0;
    int                    prompt_tokens     = 0;
    int                    output_tokens     = 0;
    long                   load_ms           = 0;  ///< JIT swap cost for this turn
};

/// A file edit an expert wants to make, waiting on an answer.
///
/// Present in a snapshot exactly while the engine is parked on it. The renderer
/// draws `before` and `after` side by side and the user picks one; the engine
/// is blocked on a condition variable the whole time, which is what makes this
/// a gate rather than a notification.
///
/// Both sides are carried whole rather than as a diff. A diff is the right way
/// to *review* a change and the wrong way to *choose* one: it shows what moved
/// and hides what the file becomes, and the question here is which of two files
/// you want on disk.
struct PendingEdit {
    std::string path;    ///< relative to the project root, as the expert named it
    std::string before;  ///< the file as it is; empty when the expert is creating it
    std::string after;   ///< the file as it would be
};

/// A consistent copy of everything the renderer needs for one frame.
struct Snapshot {
    Mood        mood = Mood::Idle;
    std::string status;

    /// Who the seats are, for this frame.
    ///
    /// A shared pointer rather than a copy: the roster is a dozen structs of
    /// strings, and copying it into every frame at eleven frames a second to
    /// draw ten labels would be pure waste. It is immutable once published, so
    /// the renderer can read it without a lock, and `/newexpert` publishes a
    /// new one rather than editing this.
    std::shared_ptr<const Roster>       roster;

    /// Parallel to `roster->experts()`.
    std::vector<SeatState>              seats;
    std::optional<ExpertId>             resident;

    /// The expert this turn is flowing to, from the moment the delegator names
    /// it until the answer is finished. What the expert panel draws the line to,
    /// and what makes a seat's dot light up -- residency is a different
    /// question, and the status bar is where that is answered.
    std::optional<ExpertId>             linked;

    /// Is the delegator in memory and ready to route?
    ///
    /// Always true while it is set to stay loaded. With it set to load on
    /// demand this goes dark while an expert has the card and comes back when
    /// the delegator does, which is the honest picture of a machine that can
    /// only hold one of them at a time.
    bool delegator_ready = false;
    std::vector<Turn>                   turns;
    std::vector<std::string>            notices;
    bool                                busy = false;

    /// How full the expert's context was on the last turn, and how big it is.
    ///
    /// Reported rather than estimated: it is what the expert's own tokenizer
    /// made of the conversation, through the expert's own chat template. Zero
    /// until a turn has run, because until then there is no model to ask.
    int context_used  = 0;
    int context_size  = 0;

    /// Tokens spent since Crucible started.
    TokenUsage session_usage;

    /// Tokens this project has ever spent, loaded from disk at startup and
    /// kept in step as the session goes on.
    TokenUsage project_usage;

    /// The rate of the reply currently streaming, or 0 when nothing is. Shown
    /// in preference to the session average, because while an answer is
    /// arriving that is the number being asked about.
    double live_tokens_per_second = 0.0;

    /// The edit waiting on an answer, or nothing. See PendingEdit.
    std::shared_ptr<const PendingEdit> pending_edit;

    /// The cook in progress, or nothing.
    ///
    /// A shared pointer for the same reason the roster is one: it grows to
    /// hundreds of steps over an hour, and copying that into every frame to
    /// draw the last twenty of them would be the most expensive thing on the
    /// screen. The engine publishes a new one after each step; the renderer
    /// reads whichever it was handed.
    std::shared_ptr<const Cook> cook;
};

class AppState {
public:
    Snapshot snapshot() const;

    void set_mood(Mood mood, std::string status = {});

    void set_seat(const ExpertId& id, SeatPhase phase, float progress = 0.0F);
    void set_seat_progress(const ExpertId& id, float progress);

    /// Adopt the config's roster and recompute every seat from it,
    /// distinguishing a seat with no model from one whose file has gone
    /// missing. Touches the filesystem, so call it on a config change rather
    /// than per frame.
    ///
    /// This is also the only way the roster the UI draws is replaced, which is
    /// what makes `/newexpert` a config change like any other.
    void configure_seats(const Config& config);

    void set_resident(std::optional<ExpertId> id);

    /// Open a new turn and return its index.
    std::size_t begin_turn(std::string prompt);

    /// Append an already-finished turn, as `/resume` does. Its tokens are not
    /// added to the session total: they were spent in an earlier session and
    /// are already in the project total.
    void restore_turn(Turn turn);
    void set_route(std::size_t turn, RouteDecision route);
    void append_reply(std::size_t turn, std::string_view chunk);

    /// Append to a turn's reasoning. See Turn::reasoning.
    void append_reasoning(std::size_t turn, std::string_view chunk);

    /// Replace a turn's reply. Used when a round of generation turns out to
    /// have been a tool request rather than an answer.
    void set_reply(std::size_t turn, std::string text);

    /// Record something this turn did. See Turn::actions.
    void add_action(std::size_t turn, TurnAction action);

    /// The expert work is flowing to, or nothing between turns.
    void set_linked(std::optional<ExpertId> id);

    /// Whether the delegator is loaded and able to route.
    void set_delegator_ready(bool ready);
    void finish_turn(std::size_t turn, const GenerationStats& stats, long load_ms);
    void fail_turn(std::size_t turn, std::string_view reason);

    /// Throw one turn away.
    ///
    /// The transcript is the record of a conversation, and a record you cannot
    /// correct is a record you stop trusting -- a question asked badly, or an
    /// answer that went somewhere useless, is worth being able to remove rather
    /// than scroll past forever. The caller is responsible for putting the
    /// engine's own history back in step; see App::rebuild_history.
    void remove_turn(std::size_t turn);

    /// Throw `from` and everything after it away.
    ///
    /// What re-asking a question needs: the turns that followed were answers in
    /// a conversation that is about to be different, and leaving them would
    /// show the model's replies to a context it never saw.
    void truncate_turns(std::size_t from);

    /// Put an edit up for approval, or take it back down with nothing.
    void set_pending_edit(std::shared_ptr<const PendingEdit> edit);

    /// How full the context was on the last turn. See Snapshot::context_used.
    void set_context_used(int used, int size);

    /// The edit waiting on an answer, without copying a whole snapshot to ask.
    /// The terminal checks this on every keystroke.
    std::shared_ptr<const PendingEdit> pending_edit() const;

    /// Close a turn the user stopped.
    ///
    /// Apart from fail_turn because a stop is not a failure: nothing went
    /// wrong, there is nothing to explain, and the turn should read as one the
    /// user ended rather than one that broke. Reached when a stop lands during
    /// a model load, which is the one part of a turn that has no tokens to
    /// stop mid-flight.
    void cancel_turn(std::size_t turn);

    /// Report the rate of the reply in flight. Called every few tokens by the
    /// engine, and reset to 0 when the turn ends.
    void set_live_rate(double tokens_per_second);

    /// Seed the project total from disk. Called once, at startup.
    void set_project_usage(TokenUsage usage);

    void add_notice(std::string notice);
    void clear_notices();
    void clear_turns();

    void set_busy(bool busy);
    bool busy() const;

    /// Publish the cook in progress, or nothing when one ends.
    void set_cook(std::shared_ptr<const Cook> cook);
    std::shared_ptr<const Cook> cook() const;

private:
    /// Index of `id` in the current roster, or nullopt. Call with the lock
    /// held; it does not take one.
    std::optional<std::size_t> seat_index(const ExpertId& id) const;

    mutable std::mutex                   mutex_;
    Mood                                 mood_ = Mood::Idle;
    std::string                          status_;
    std::shared_ptr<const Roster>        roster_ =
        std::make_shared<const Roster>(Roster::bare());
    std::vector<SeatState>               seats_ =
        std::vector<SeatState>(roster_->size());
    std::optional<ExpertId>              resident_;
    std::optional<ExpertId>              linked_;
    bool                                 delegator_ready_ = false;
    std::vector<Turn>                    turns_;
    std::vector<std::string>             notices_;
    bool                                 busy_ = false;
    std::shared_ptr<const Cook>          cook_;
    std::shared_ptr<const PendingEdit>   pending_edit_;
    int                                  context_used_ = 0;
    int                                  context_size_ = 0;
    TokenUsage                           session_usage_;
    TokenUsage                           project_usage_;
    double                               live_rate_ = 0.0;
};

}  // namespace crucible
