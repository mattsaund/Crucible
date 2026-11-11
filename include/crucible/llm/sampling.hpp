// SPDX-License-Identifier: MIT
//
// Building a llama.cpp sampler chain from Crucible's configuration.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "crucible/config/config.hpp"

struct llama_sampler;
struct llama_vocab;

namespace crucible::llm {

/// A grammar on its own, outside any chain, for a caller that applies it
/// first and sets it aside where it should not hold (LoadedModel::
/// sample_shaped). A lazy one holds nothing until the answer so far matches
/// one of `patterns` -- regular expressions -- or one of `tokens` is sampled,
/// and from there holds what follows. Null when `grammar` does not parse.
llama_sampler* build_grammar_sampler(const llama_vocab* vocab, const std::string& grammar, bool lazy,
                                     const std::vector<std::string>& patterns,
                                     const std::vector<std::int32_t>& tokens);

/// Assemble the sampler chain for one generation.
///
/// Order is deliberate. A grammar, when present, goes in first so it filters
/// the candidate set before any truncation sampler runs -- otherwise top-k
/// could discard the only tokens the grammar would have allowed, leaving the
/// sampler with nothing legal to pick.
///
/// The caller owns the result and must free it with `llama_sampler_free`.
llama_sampler* build_sampler_chain(const llama_vocab* vocab,
                                   const ModelParams& params,
                                   const std::string& grammar);

}  // namespace crucible::llm
