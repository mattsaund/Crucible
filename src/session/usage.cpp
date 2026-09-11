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

std::string format_tokens(std::uint64_t count) {
    if (count < 1000) {
        return std::to_string(count);
    }
    if (count < 1000000) {
        return format::number(static_cast<double>(count) / 1000.0, 1) + "k";
    }
    return format::number(static_cast<double>(count) / 1000000.0, 1) + "M";
}

std::string usage_readout(const TokenUsage& usage, double live_tps, bool unicode) {
    // One "tok" in front labels both counts without repeating itself, and
    // keeps the status bar narrow enough to leave room for the key hints.
    std::string text = unicode
        ? "tok ↑ " + format_tokens(usage.input_tokens) +
          "  ↓ " + format_tokens(usage.output_tokens)
        : "tok in " + format_tokens(usage.input_tokens) +
          "  out " + format_tokens(usage.output_tokens);

    const double rate = live_tps > 0.0 ? live_tps : usage.tokens_per_second();
    if (rate > 0.0) {
        text += unicode ? "  ·  " : "  |  ";
        text += format::number(rate, 1) + " tok/s";
    }
    return text;
}

}  // namespace crucible
