// SPDX-License-Identifier: MIT
//
// Token arithmetic and how it is displayed.
#include "crucible/session/usage.hpp"

#include <algorithm>
#include <cmath>

#include "crucible/llm/loaded_model.hpp"
#include "crucible/util/format.hpp"

namespace crucible {

void TokenUsage::add(const GenerationStats& stats) {
    input_tokens  += static_cast<std::uint64_t>(std::max(0, stats.prompt_tokens));
    output_tokens += static_cast<std::uint64_t>(std::max(0, stats.output_tokens));
    output_ms     += stats.output_ms;
    ++turns;
}

void TokenUsage::add(const TokenUsage& other) {
    input_tokens  += other.input_tokens;
    output_tokens += other.output_tokens;
    output_ms     += other.output_ms;
    turns         += other.turns;
}

double TokenUsage::tokens_per_second() const {
    if (output_ms <= 0.0 || output_tokens == 0) {
        return 0.0;
    }
    return static_cast<double>(output_tokens) / (output_ms / 1000.0);
}

}  // namespace crucible
