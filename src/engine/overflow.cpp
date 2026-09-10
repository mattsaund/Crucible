// SPDX-License-Identifier: MIT
#include "crucible/engine/overflow.hpp"

namespace crucible {

std::size_t trim_to_budget(Overflow policy, std::vector<ChatMessage>& messages,
                           const TokenCounter& count, int budget) {
    if (policy == Overflow::StopAtLimit || budget <= 0 || messages.size() < 3
        || !count) {
        return 0;
    }

    std::size_t dropped = 0;
    // Two spare either end: the system prompt at the front and the question at
    // the back. Everything between them is exchanges, and there has to be a
    // whole one left to take.
    while (messages.size() >= 4 && count(messages) > budget) {
        const std::size_t history = messages.size() - 2;
        std::size_t at = 1;                       // just past the system prompt
        if (policy == Overflow::TruncateMiddle) {
            // Out of the middle, so the opening survives. Rounded down to an
            // even offset from the first history message, or the cut would
            // straddle a pair and leave an answer with no question.
            at = 1 + (history / 2 / 2) * 2;
            if (at + 2 > messages.size() - 1) {
                at = 1;                           // no whole pair there; take the front
            }
        }
        messages.erase(messages.begin() + static_cast<long>(at),
                       messages.begin() + static_cast<long>(at + 2));
        dropped += 2;
    }
    return dropped;
}

}  // namespace crucible
