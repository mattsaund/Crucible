// SPDX-License-Identifier: MIT
#include "crucible/llm/sampling.hpp"

#include <cstdint>
#include <random>

#include <llama.h>

namespace crucible::llm {

llama_sampler* build_sampler_chain(const llama_vocab* vocab,
                                   const ModelParams& params,
                                   const std::string& grammar) {
    llama_sampler* chain = llama_sampler_chain_init(llama_sampler_chain_default_params());

    if (!grammar.empty()) {
        // First, so the grammar filters the candidate set before any truncation
        // sampler can throw away the only legal tokens.
        if (llama_sampler* g = llama_sampler_init_grammar(vocab, grammar.c_str(), "root");
            g != nullptr) {
            llama_sampler_chain_add(chain, g);
        }
    }

    if (params.repeat_penalty != 1.0F) {
        llama_sampler_chain_add(chain, llama_sampler_init_penalties(
            llama_vocab_n_tokens(vocab), params.repeat_last_n, params.repeat_penalty,
            /*penalty_freq=*/0.0F, /*penalty_present=*/0.0F));
    }

    if (params.temperature <= 0.0F) {
        // Greedy. Routing uses this so the same question always reaches the
        // same expert, and so a benchmark number means something.
        llama_sampler_chain_add(chain, llama_sampler_init_greedy());
        return chain;
    }

    llama_sampler_chain_add(chain, llama_sampler_init_top_k(params.top_k));
    llama_sampler_chain_add(chain, llama_sampler_init_top_p(params.top_p, 1));
    llama_sampler_chain_add(chain, llama_sampler_init_min_p(params.min_p, 1));
    llama_sampler_chain_add(chain, llama_sampler_init_temp(params.temperature));

    std::uint32_t seed = params.seed;
    if (seed == 0xFFFFFFFFU) {
        seed = std::random_device{}();
    }
    llama_sampler_chain_add(chain, llama_sampler_init_dist(seed));
    return chain;
}

}  // namespace crucible::llm
