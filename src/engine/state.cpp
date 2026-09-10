// SPDX-License-Identifier: MIT
//
// The one piece of memory the engine thread and the UI thread share.
//
// The locking rule is simple and worth keeping that way: every public method
// takes the mutex for its whole body, and nothing calls out to unknown code
// while holding it. Filesystem work is done before the lock is taken, so the
// render thread is never blocked behind a stat() call.
#include "crucible/engine/state.hpp"

#include <algorithm>
#include <filesystem>

namespace crucible {

std::string_view mood_label(Mood mood) {
    switch (mood) {
        case Mood::Idle:     return "idle";
        case Mood::Routing:  return "routing";
        case Mood::Loading:  return "loading";
        case Mood::Thinking: return "thinking";
        case Mood::Talking:  return "answering";
        case Mood::Error:    return "error";
    }
    return "idle";
}

Snapshot AppState::snapshot() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    Snapshot copy;
    copy.mood     = mood_;
    copy.status   = status_;
    copy.roster   = roster_;
    copy.cook     = cook_;
    copy.pending_edit = pending_edit_;
    copy.seats    = seats_;
    copy.resident        = resident_;
    copy.linked          = linked_;
    copy.delegator_ready = delegator_ready_;
    copy.turns    = turns_;
    copy.notices  = notices_;
    copy.busy     = busy_;
    copy.session_usage = session_usage_;
    copy.project_usage = project_usage_;
    copy.live_tokens_per_second = live_rate_;
    return copy;
}

void AppState::set_mood(Mood mood, std::string status) {
    const std::lock_guard<std::mutex> lock(mutex_);
    mood_   = mood;
    status_ = std::move(status);
}

std::optional<std::size_t> AppState::seat_index(const ExpertId& id) const {
    return roster_ ? roster_->find(id) : std::nullopt;
}

void AppState::set_seat(const ExpertId& id, SeatPhase phase, float progress) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::optional<std::size_t> index = seat_index(id);
    // An id that is not on the roster is silently ignored rather than clamped
    // to a seat that does exist. It means a seat was ejected while work was in
    // flight, and lighting up whoever is at that index now would be a lie.
    if (!index || *index >= seats_.size()) {
        return;
    }
    seats_[*index].phase    = phase;
    seats_[*index].progress = progress;
}

void AppState::set_seat_progress(const ExpertId& id, float progress) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::optional<std::size_t> index = seat_index(id);
    if (!index || *index >= seats_.size()) {
        return;
    }
    seats_[*index].progress = progress;
}

void AppState::configure_seats(const Config& config) {
    // Resolve existence outside the lock: stat-ing a dozen files while holding
    // the mutex would block the render thread for no reason.
    auto roster = std::make_shared<const Roster>(config.roster);
    std::vector<SeatState> seats(roster->size());
    for (std::size_t i = 0; i < roster->size(); ++i) {
        const ModelParams& params = config.expert(roster->at(i).id);
        if (params.model.empty()) {
            seats[i].phase = SeatPhase::Unconfigured;
        } else if (std::filesystem::exists(params.path)) {
            seats[i].phase = SeatPhase::Dormant;
        } else {
            // Assigned but absent. Saying so is better than showing a seat as
            // ready and only failing when someone routes a prompt to it.
            seats[i].phase = SeatPhase::Missing;
        }
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    roster_ = std::move(roster);
    seats_  = std::move(seats);

    // A seat that was lit may not exist any more. Dropping the reference is
    // what stops the expert panel drawing a connector to a row that is no longer
    // there after an /ejectexpert.
    if (resident_ && !seat_index(*resident_)) {
        resident_.reset();
    }
    if (linked_ && !seat_index(*linked_)) {
        linked_.reset();
    }
}

void AppState::set_resident(std::optional<ExpertId> id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    // Demote whoever was previously lit; only one expert is ever resident.
    for (SeatState& seat : seats_) {
        if (seat.phase == SeatPhase::Active) {
            seat.phase = SeatPhase::Dormant;
        }
    }
    resident_ = std::move(id);
    if (resident_) {
        if (const std::optional<std::size_t> index = seat_index(*resident_);
            index && *index < seats_.size()) {
            seats_[*index].phase    = SeatPhase::Active;
            seats_[*index].progress = 1.0F;
        }
    }
}

void AppState::set_linked(std::optional<ExpertId> id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    linked_ = std::move(id);
    // A seat is lit while work is flowing to it and dark the moment it stops.
    // Whether the weights are still in memory afterwards is a separate fact,
    // and one the status bar already reports.
    for (SeatState& seat : seats_) {
        if (seat.phase == SeatPhase::Active) {
            seat.phase = SeatPhase::Dormant;
        }
    }
    if (linked_) {
        if (const std::optional<std::size_t> index = seat_index(*linked_);
            index && *index < seats_.size() && seats_[*index].phase == SeatPhase::Dormant) {
            seats_[*index].phase = SeatPhase::Active;
        }
    }
}

void AppState::set_cook(std::shared_ptr<const Cook> cook) {
    const std::lock_guard<std::mutex> lock(mutex_);
    cook_ = std::move(cook);
}

std::shared_ptr<const Cook> AppState::cook() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return cook_;
}

void AppState::set_delegator_ready(bool ready) {
    const std::lock_guard<std::mutex> lock(mutex_);
    delegator_ready_ = ready;
}

std::size_t AppState::begin_turn(std::string prompt) {
    const std::lock_guard<std::mutex> lock(mutex_);
    Turn turn;
    turn.prompt    = std::move(prompt);
    turn.streaming = true;
    turns_.push_back(std::move(turn));
    return turns_.size() - 1;
}

void AppState::restore_turn(Turn turn) {
    const std::lock_guard<std::mutex> lock(mutex_);
    turn.streaming = false;
    turns_.push_back(std::move(turn));
}

void AppState::set_route(std::size_t turn, RouteDecision route) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (turn < turns_.size()) {
        turns_[turn].route = std::move(route);
    }
}

void AppState::append_reply(std::size_t turn, std::string_view chunk) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (turn < turns_.size()) {
        turns_[turn].reply.append(chunk);
    }
}

void AppState::append_reasoning(std::size_t turn, std::string_view chunk) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (turn < turns_.size()) {
        turns_[turn].reasoning.append(chunk);
    }
}

void AppState::set_reply(std::size_t turn, std::string text) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (turn < turns_.size()) {
        turns_[turn].reply = std::move(text);
    }
}

void AppState::add_action(std::size_t turn, std::string line) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (turn < turns_.size()) {
        turns_[turn].actions.push_back(std::move(line));
    }
}

void AppState::finish_turn(std::size_t turn, const GenerationStats& stats, long load_ms) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (turn >= turns_.size()) {
        return;
    }
    Turn& entry = turns_[turn];
    entry.streaming         = false;
    entry.canceled         = stats.canceled;
    entry.tokens_per_second = stats.tokens_per_second();
    entry.prompt_tokens     = stats.prompt_tokens;
    entry.output_tokens     = stats.output_tokens;
    entry.load_ms           = load_ms;

    // The session total counts every generation, canceled ones included --
    // the tokens were produced either way, and hiding them would make the
    // readout disagree with what the machine actually did.
    session_usage_.add(stats);
    project_usage_.add(stats);
    live_rate_ = 0.0;
}

void AppState::set_live_rate(double tokens_per_second) {
    const std::lock_guard<std::mutex> lock(mutex_);
    live_rate_ = tokens_per_second;
}

void AppState::set_project_usage(TokenUsage usage) {
    const std::lock_guard<std::mutex> lock(mutex_);
    project_usage_ = usage;
}

void AppState::fail_turn(std::size_t turn, std::string_view reason) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (turn >= turns_.size()) {
        return;
    }
    Turn& entry = turns_[turn];
    entry.streaming = false;
    entry.failed    = true;
    live_rate_      = 0.0;
    if (entry.reply.empty()) {
        entry.reply = reason;
    }
}

void AppState::remove_turn(std::size_t turn) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (turn >= turns_.size()) {
        return;
    }
    turns_.erase(turns_.begin() + static_cast<long>(turn));
}

void AppState::truncate_turns(std::size_t from) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (from >= turns_.size()) {
        return;
    }
    turns_.erase(turns_.begin() + static_cast<long>(from), turns_.end());
}

void AppState::set_pending_edit(std::shared_ptr<const PendingEdit> edit) {
    const std::lock_guard<std::mutex> lock(mutex_);
    pending_edit_ = std::move(edit);
}

void AppState::cancel_turn(std::size_t turn) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (turn >= turns_.size()) {
        return;
    }
    Turn& entry = turns_[turn];
    entry.streaming = false;
    entry.canceled = true;
    live_rate_      = 0.0;
}

void AppState::add_notice(std::string notice) {
    const std::lock_guard<std::mutex> lock(mutex_);
    // Startup can produce one warning per misconfigured expert; keep the list
    // bounded so a badly broken config cannot push the chat off screen.
    if (notices_.size() < 32) {
        notices_.push_back(std::move(notice));
    }
}

void AppState::clear_notices() {
    const std::lock_guard<std::mutex> lock(mutex_);
    notices_.clear();
}

void AppState::clear_turns() {
    const std::lock_guard<std::mutex> lock(mutex_);
    turns_.clear();
}

void AppState::set_busy(bool busy) {
    const std::lock_guard<std::mutex> lock(mutex_);
    busy_ = busy;
}

bool AppState::busy() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return busy_;
}

}  // namespace crucible
