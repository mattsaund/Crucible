// SPDX-License-Identifier: MIT
//
// Making a conversation fit in a context window.
//
// The failure this guards against is a quiet one. Drop the wrong pair and the
// transcript still looks like a transcript; drop half a pair and the model sees
// an answer with no question, which reads to it as something it got wrong. In
// both cases the replies get worse for a reason nobody can see, because nothing
// on screen says the start of the conversation is gone.
//
// Counted in fake tokens -- one per word -- so the three strategies can be
// checked without thirty gigabytes of weights. What the engine passes instead
// is the expert's own tokenizer through its own chat template, which is the
// only figure that means anything in production; the arithmetic being checked
// here is the same either way.
#include "test_helpers.hpp"

#include "crucible/engine/overflow.hpp"

namespace {

using crucible::ChatMessage;
using crucible::Overflow;
using crucible::TokenCounter;
using crucible::trim_to_budget;

/// One "token" per message, which makes a budget a message count and every
/// expectation below readable as one.
TokenCounter by_message() {
    return [](const std::vector<ChatMessage>& messages) {
        return static_cast<int>(messages.size());
    };
}

/// [system, q1, a1, q2, a2, ..., new question]
std::vector<ChatMessage> conversation(int exchanges) {
    std::vector<ChatMessage> messages{{"system", "you are an expert"}};
    for (int i = 1; i <= exchanges; ++i) {
        messages.push_back({"user", "q" + std::to_string(i)});
        messages.push_back({"assistant", "a" + std::to_string(i)});
    }
    messages.push_back({"user", "the new question"});
    return messages;
}

std::vector<std::string> texts(const std::vector<ChatMessage>& messages) {
    std::vector<std::string> out;
    out.reserve(messages.size());
    for (const ChatMessage& message : messages) {
        out.push_back(message.content);
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------

TEST(a_conversation_that_fits_is_left_alone) {
    std::vector<ChatMessage> messages = conversation(3);   // 8 messages
    const std::size_t dropped =
        trim_to_budget(Overflow::RollingWindow, messages, by_message(), 20);
    CHECK_EQ(dropped, std::size_t{0});
    CHECK_EQ(messages.size(), std::size_t{8});
}

TEST(a_rolling_window_drops_the_oldest_first) {
    std::vector<ChatMessage> messages = conversation(4);   // 10 messages
    const std::size_t dropped =
        trim_to_budget(Overflow::RollingWindow, messages, by_message(), 6);
    CHECK_EQ(dropped, std::size_t{4});

    const std::vector<std::string> left = texts(messages);
    CHECK_EQ(left.size(), std::size_t{6});
    // The instructions and the question survive; the two oldest exchanges went.
    CHECK_EQ(left.front(), std::string("you are an expert"));
    CHECK_EQ(left.back(), std::string("the new question"));
    CHECK_EQ(left[1], std::string("q3"));
    CHECK_EQ(left[2], std::string("a3"));
    CHECK_EQ(left[3], std::string("q4"));
}

TEST(truncating_the_middle_keeps_both_ends) {
    // The case a rolling window gets wrong: a conversation that opened with
    // something that has to survive -- a specification, a file, a set of rules
    // -- and has since wandered.
    std::vector<ChatMessage> messages = conversation(4);   // 10 messages
    const std::size_t dropped =
        trim_to_budget(Overflow::TruncateMiddle, messages, by_message(), 8);
    CHECK_EQ(dropped, std::size_t{2});

    const std::vector<std::string> left = texts(messages);
    CHECK_EQ(left.size(), std::size_t{8});
    // The opening exchange is still there, which is the whole point.
    CHECK_EQ(left[1], std::string("q1"));
    CHECK_EQ(left[2], std::string("a1"));
    // And so is the most recent one.
    CHECK_EQ(left[left.size() - 2], std::string("a4"));
    CHECK_EQ(left.back(), std::string("the new question"));
}

TEST(stopping_at_the_limit_drops_nothing_at_all) {
    // The caller refuses the turn instead. Anything else here would be the
    // silent shortening this option exists to avoid.
    std::vector<ChatMessage> messages = conversation(4);
    const std::size_t before = messages.size();
    const std::size_t dropped =
        trim_to_budget(Overflow::StopAtLimit, messages, by_message(), 2);
    CHECK_EQ(dropped, std::size_t{0});
    CHECK_EQ(messages.size(), before);
}

TEST(exchanges_are_always_dropped_whole) {
    // Half an exchange is a question with no answer or an answer with no
    // question. Checked across every budget rather than at one: an off-by-one
    // in the cut shows up at exactly one size and nowhere else.
    for (int budget = 2; budget <= 12; ++budget) {
        for (const Overflow policy : {Overflow::RollingWindow, Overflow::TruncateMiddle}) {
            std::vector<ChatMessage> messages = conversation(5);
            trim_to_budget(policy, messages, by_message(), budget);

            CHECK_EQ(messages.front().role, std::string("system"));
            CHECK_EQ(messages.back().content, std::string("the new question"));
            // Everything between the two is user/assistant, in that order.
            for (std::size_t i = 1; i + 1 < messages.size(); ++i) {
                const bool user = (i % 2) == 1;
                CHECK_EQ(messages[i].role, std::string(user ? "user" : "assistant"));
            }
        }
    }
}

TEST(nothing_is_dropped_past_the_last_whole_exchange) {
    // A budget nothing can satisfy must stop rather than eat the system prompt
    // or the question. Left to run, the loop would strip the conversation to
    // nothing and the turn would go out with no instructions at all.
    std::vector<ChatMessage> messages = conversation(3);
    trim_to_budget(Overflow::RollingWindow, messages, by_message(), 1);
    CHECK_EQ(messages.size(), std::size_t{2});
    CHECK_EQ(messages.front().role, std::string("system"));
    CHECK_EQ(messages.back().content, std::string("the new question"));
}

TEST(a_conversation_with_no_history_is_never_touched) {
    std::vector<ChatMessage> messages{{"system", "rules"}, {"user", "hello"}};
    for (const Overflow policy : {Overflow::RollingWindow, Overflow::TruncateMiddle,
                                  Overflow::StopAtLimit}) {
        std::vector<ChatMessage> copy = messages;
        CHECK_EQ(trim_to_budget(policy, copy, by_message(), 1), std::size_t{0});
        CHECK_EQ(copy.size(), std::size_t{2});
    }
}

TEST(the_three_strategies_round_trip_through_their_names) {
    // The config stores a word so the file stays readable, and an unknown word
    // has to fall back to the careful default rather than to whatever integer
    // happened to be there.
    for (const Overflow policy : {Overflow::RollingWindow, Overflow::TruncateMiddle,
                                  Overflow::StopAtLimit}) {
        CHECK(crucible::overflow_from_id(crucible::overflow_id(policy)) == policy);
    }
    CHECK(crucible::overflow_from_id("nonsense") == Overflow::RollingWindow);
    CHECK(crucible::overflow_from_id("") == Overflow::RollingWindow);
}
