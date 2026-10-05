// SPDX-License-Identifier: MIT
//
// Counting tokens.
//
// Three scopes, because they answer different questions:
//   turn    -- how fast was that reply?          (tok/s, shown as it streams)
//   session -- what has this conversation cost?  (since Crucible started)
//   project -- what has this codebase cost?      (across every session, ever)
//
// Nothing here is billed -- Crucible runs locally. The numbers are for
// understanding: which expert is slow, whether an answer is about to run out
// of context, and how much work a project has actually taken.
#pragma once

#include <cstdint>
#include <string>

namespace crucible {

struct GenerationStats;

/// Tokens in, tokens out, and the time it took.
struct TokenUsage {
    std::uint64_t input_tokens  = 0;  ///< prompt tokens fed to a model
    std::uint64_t output_tokens = 0;  ///< tokens generated
    std::uint64_t turns         = 0;

    /// Time spent generating output, in milliseconds. Prompt ingestion is not
    /// counted: including it would make tok/s depend on prompt length, which
    /// hides the number people actually want to compare between experts.
    double output_ms = 0.0;

    void add(const GenerationStats& stats);
    void add(const TokenUsage& other);

    std::uint64_t total_tokens() const { return input_tokens + output_tokens; }

    /// Output tokens per second across everything counted here, or 0.
    double tokens_per_second() const;
};

}  // namespace crucible
