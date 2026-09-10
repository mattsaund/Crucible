// SPDX-License-Identifier: MIT
//
// Editing the transcript: stopping a turn, deleting one, and re-asking one.
//
// These are index operations on a vector, which is exactly why they are worth
// pinning. A delete that removes the wrong entry, or an off-by-one in the
// truncate that a re-ask does, produces a transcript that still looks
// plausible -- the questions are all real questions and the answers are all
// real answers, they have simply stopped belonging to each other. Nothing about
// that is visible until somebody reads back through it.
//
// The other half of each of these operations is putting the engine's own
// conversation history back in step, which lives in the desktop app. What is
// checkable here is the rule it follows: only exchanges that actually produced
// an answer are worth handing back to an expert.
#include "test_helpers.hpp"

#include "crucible/engine/state.hpp"

namespace {

using crucible::AppState;
using crucible::Snapshot;
using crucible::Turn;

/// Three finished turns, numbered so a misplaced one is obvious.
///
/// Filled in place rather than returned: AppState owns the mutex the engine
/// and the renderer meet across, so it is neither copyable nor movable.
void three_turns(AppState& state) {
    for (int i = 1; i <= 3; ++i) {
        Turn turn;
        turn.prompt = "question " + std::to_string(i);
        turn.reply  = "answer " + std::to_string(i);
        state.restore_turn(turn);
    }
}

std::vector<std::string> prompts_of(const AppState& state) {
    std::vector<std::string> out;
    for (const Turn& turn : state.snapshot().turns) {
        out.push_back(turn.prompt);
    }
    return out;
}

/// The rule App::rebuild_history follows, so the reason for it can be checked
/// without a window: an expert is told about exchanges that happened, and a
/// turn that failed or was stopped did not happen.
std::size_t history_pairs(const AppState& state) {
    std::size_t pairs = 0;
    for (const Turn& turn : state.snapshot().turns) {
        if (!turn.failed && !turn.canceled && !turn.reply.empty()) {
            ++pairs;
        }
    }
    return pairs;
}

}  // namespace

// ---------------------------------------------------------------------------

TEST(deleting_a_turn_takes_the_one_that_was_asked_for) {
    AppState state;
    three_turns(state);
    state.remove_turn(1);

    const std::vector<std::string> left = prompts_of(state);
    CHECK_EQ(left.size(), std::size_t{2});
    // The one before it and the one after it, still in order and still
    // themselves. A delete that shuffled these would leave a transcript that
    // reads perfectly and says something nobody wrote.
    CHECK_EQ(left[0], std::string("question 1"));
    CHECK_EQ(left[1], std::string("question 3"));
}

TEST(deleting_past_the_end_does_nothing_rather_than_something_worse) {
    // The transcript is edited from a frame that was drawn against a snapshot,
    // and a reply can finish between the two. An index that has gone stale must
    // land on nothing at all.
    AppState state;
    three_turns(state);
    state.remove_turn(3);
    state.remove_turn(99);
    CHECK_EQ(prompts_of(state).size(), std::size_t{3});
}

TEST(re_asking_drops_the_turns_that_answered_a_different_conversation) {
    // Asking question 2 again means question 2 and everything after it goes.
    // The replies that followed were answers in a conversation that is now
    // going to be a different one; leaving them would show an expert's replies
    // to a context it never saw.
    AppState state;
    three_turns(state);
    state.truncate_turns(1);

    const std::vector<std::string> left = prompts_of(state);
    CHECK_EQ(left.size(), std::size_t{1});
    CHECK_EQ(left[0], std::string("question 1"));
}

TEST(re_asking_the_last_turn_leaves_the_ones_before_it) {
    AppState state;
    three_turns(state);
    state.truncate_turns(2);
    CHECK_EQ(prompts_of(state).size(), std::size_t{2});

    // And truncating at the end is a no-op, not an empty transcript.
    state.truncate_turns(2);
    CHECK_EQ(prompts_of(state).size(), std::size_t{2});
    state.truncate_turns(99);
    CHECK_EQ(prompts_of(state).size(), std::size_t{2});
}

TEST(a_stopped_turn_is_not_a_failed_one) {
    AppState state;
    three_turns(state);
    state.cancel_turn(1);

    const Snapshot snapshot = state.snapshot();
    CHECK(snapshot.turns[1].canceled);
    // Stopping is not an error and must not be dressed as one: "could not
    // load, turn off GPU-only compute" is bad advice to give somebody who
    // pressed stop.
    CHECK(!snapshot.turns[1].failed);
    CHECK(!snapshot.turns[1].streaming);
    // And it leaves what had already arrived alone. A reply cut off halfway is
    // still half an answer, and throwing it away on the way out is a second
    // loss on top of the one the user asked for.
    CHECK_EQ(snapshot.turns[1].reply, std::string("answer 2"));
}

TEST(only_exchanges_that_happened_go_back_to_the_expert) {
    AppState state;
    three_turns(state);
    CHECK_EQ(history_pairs(state), std::size_t{3});

    // A stopped turn drops out of the context. Left in, it teaches the model
    // that trailing off mid-answer is a thing that happens in this
    // conversation -- and models are obliging about that sort of hint.
    state.cancel_turn(0);
    CHECK_EQ(history_pairs(state), std::size_t{2});

    state.fail_turn(1, "the card ran out of memory");
    CHECK_EQ(history_pairs(state), std::size_t{1});

    // Deleting the last good one leaves nothing to tell the expert about.
    state.remove_turn(2);
    CHECK_EQ(history_pairs(state), std::size_t{0});
}
