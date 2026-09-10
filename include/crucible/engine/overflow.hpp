// SPDX-License-Identifier: MIT
//
// Making a conversation fit in a context window.
//
// Separate from the engine because it is arithmetic on a list, and the one
// thing that must not be wrong about it: dropping the wrong pair, or dropping
// half a pair, produces a conversation that still looks like a conversation and
// has quietly stopped making sense. Nothing on screen would say so.
//
// Takes a counting function rather than a model, so what it does can be checked
// without thirty gigabytes of weights. The engine passes the expert's own
// tokenizer through its own chat template, which is the only figure that means
// anything -- a character count is out by a factor of three or four depending
// on the tokenizer and the language.
#pragma once

#include <cstddef>
#include <functional>
#include <vector>

#include "crucible/config/config.hpp"
#include "crucible/llm/loaded_model.hpp"

namespace crucible {

/// How many tokens a conversation costs, whole.
using TokenCounter = std::function<int(const std::vector<ChatMessage>&)>;

/// Drop exchanges from `messages` until `count` reports it fits in `budget`.
///
/// `messages` is [system, ...history..., user]: the first and the last are
/// never candidates. The system prompt is the expert's instructions and
/// dropping it changes what the model is; the last message is the question
/// being asked, and a turn without it is not the turn.
///
/// Exchanges go in user/assistant pairs. Half an exchange is a question with no
/// answer or an answer with no question, and both read to the model as
/// something it got wrong.
///
/// StopAtLimit drops nothing and returns zero -- the caller refuses the turn
/// instead, which is the whole point of it.
///
/// Returns how many *messages* were dropped, so the caller can say so. Silence
/// is the failure mode that matters here.
std::size_t trim_to_budget(Overflow policy, std::vector<ChatMessage>& messages,
                           const TokenCounter& count, int budget);

}  // namespace crucible
