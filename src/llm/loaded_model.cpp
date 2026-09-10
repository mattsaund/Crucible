// SPDX-License-Identifier: MIT
//
// Generating text from a loaded model.
//
// The loop is deliberately plain: clear the KV cache, feed the whole prompt,
// then sample a token at a time. Re-feeding the conversation each turn is
// wasteful, but an expert swap invalidates the cache anyway, and correctness
// here is worth more than the saving. Prefix reuse is a later optimization.
#include "crucible/llm/loaded_model.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include <llama.h>

#include "crucible/llm/sampling.hpp"
#include "crucible/util/text.hpp"

namespace crucible {
namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string piece_for(const llama_vocab* vocab, llama_token token) {
    std::string piece(64, '\0');
    int written = llama_token_to_piece(vocab, token, piece.data(),
                                       static_cast<int32_t>(piece.size()), 0, false);
    if (written < 0) {
        piece.resize(static_cast<std::size_t>(-written));
        written = llama_token_to_piece(vocab, token, piece.data(),
                                       static_cast<int32_t>(piece.size()), 0, false);
    }
    piece.resize(written > 0 ? static_cast<std::size_t>(written) : 0);
    return piece;
}

std::vector<llama_token> tokenize(const llama_vocab* vocab, const std::string& text,
                                  bool add_special) {
    // A negative return is llama.cpp telling us how much room it actually needs.
    int needed = -llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()),
                                 nullptr, 0, add_special, true);
    if (needed <= 0) {
        return {};
    }
    std::vector<llama_token> tokens(static_cast<std::size_t>(needed));
    const int written = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()),
                                       tokens.data(), needed, add_special, true);
    tokens.resize(written > 0 ? static_cast<std::size_t>(written) : 0);
    return tokens;
}

}  // namespace

// ---------------------------------------------------------------------------
// GenerationStats
// ---------------------------------------------------------------------------

double GenerationStats::tokens_per_second() const {
    if (output_ms <= 0.0 || output_tokens <= 0) {
        return 0.0;
    }
    return static_cast<double>(output_tokens) * 1000.0 / output_ms;
}

// ---------------------------------------------------------------------------
// LoadedModel
// ---------------------------------------------------------------------------

LoadedModel::LoadedModel(llama_model* model, llama_context* ctx, std::string path)
    : model_(model), ctx_(ctx), path_(std::move(path)) {}

LoadedModel::~LoadedModel() {
    if (ctx_ != nullptr) {
        llama_free(ctx_);
    }
    if (model_ != nullptr) {
        llama_model_free(model_);
    }
}

int LoadedModel::count_tokens(const std::string& text) const {
    const llama_vocab* vocab = llama_model_get_vocab(model_);
    if (vocab == nullptr || text.empty()) {
        return 0;
    }
    return static_cast<int>(tokenize(vocab, text, false).size());
}

int LoadedModel::context_size() const {
    return static_cast<int>(llama_n_ctx(ctx_));
}

std::uint64_t LoadedModel::params() const { return llama_model_n_params(model_); }
std::uint64_t LoadedModel::bytes()  const { return llama_model_size(model_); }

std::string LoadedModel::format_chat(const std::vector<ChatMessage>& messages,
                                     bool add_assistant_prefix) const {
    std::vector<llama_chat_message> native;
    native.reserve(messages.size());
    for (const ChatMessage& message : messages) {
        native.push_back({message.role.c_str(), message.content.c_str()});
    }

    // A GGUF without an embedded template still has to be usable, so fall back
    // to ChatML rather than refusing to run the model.
    const char* tmpl = llama_model_chat_template(model_, nullptr);
    if (tmpl == nullptr) {
        tmpl = "chatml";
    }

    std::size_t capacity = 0;
    for (const ChatMessage& message : messages) {
        capacity += message.role.size() + message.content.size() + 32;
    }
    std::string buffer(std::max<std::size_t>(capacity * 2, 1024), '\0');

    int written = llama_chat_apply_template(tmpl, native.data(), native.size(),
                                            add_assistant_prefix, buffer.data(),
                                            static_cast<int32_t>(buffer.size()));
    if (written > static_cast<int>(buffer.size())) {
        buffer.resize(static_cast<std::size_t>(written));
        written = llama_chat_apply_template(tmpl, native.data(), native.size(),
                                            add_assistant_prefix, buffer.data(),
                                            static_cast<int32_t>(buffer.size()));
    }
    if (written < 0) {
        // Last resort: a plain transcript. Worse quality than the real
        // template, but it still produces an answer.
        std::string plain;
        for (const ChatMessage& message : messages) {
            plain += message.role + ": " + message.content + "\n";
        }
        if (add_assistant_prefix) {
            plain += "assistant: ";
        }
        return plain;
    }

    buffer.resize(static_cast<std::size_t>(written));
    return buffer;
}

namespace detail {

std::size_t reusable_prefix(const std::vector<llama_token>& cached,
                            const std::vector<llama_token>& wanted) {
    std::size_t common = 0;
    while (common < cached.size() && common < wanted.size() &&
           cached[common] == wanted[common]) {
        ++common;
    }
    if (common == wanted.size() && common > 0) {
        --common;  // see the header: never the whole prompt
    }
    return common;
}

}  // namespace detail

std::size_t LoadedModel::reuse_prefix(const std::vector<llama_token>& tokens) {
    llama_memory_t memory = llama_get_memory(ctx_);

    const std::size_t common = detail::reusable_prefix(cached_, tokens);
    if (common > 0) {
        // Nothing to remove when the new prompt simply continues the old one,
        // which is what a conversation growing by a turn looks like.
        //
        // Otherwise the tail has to go, and that can fail: a recurrent model
        // keeps a state rather than a per-token cache, and a state cannot be
        // rewound to an arbitrary point. llama.cpp says so by returning false,
        // and the honest answer to that is to start again from cold.
        if (common == cached_.size() ||
            llama_memory_seq_rm(memory, 0, static_cast<llama_pos>(common), -1)) {
            cached_.resize(common);
            return common;
        }
    }

    llama_memory_clear(memory, true);
    cached_.clear();
    return 0;
}

std::vector<float> LoadedModel::score_labels(const std::string& prompt,
                                             const std::vector<std::string>& labels,
                                             const CancelCallback& cancel) {
    std::vector<float> scores(labels.size(), kUnscored);
    if (labels.empty()) {
        return scores;
    }

    const llama_vocab* vocab = llama_model_get_vocab(model_);
    llama_memory_t     memory = llama_get_memory(ctx_);
    // Scoring leaves the cache holding labels rather than a conversation, so
    // the prefix bookkeeping is not valid across it either way.
    llama_memory_clear(memory, true);
    cached_.clear();

    std::vector<llama_token> tokens = tokenize(vocab, prompt, true);
    if (tokens.empty()) {
        return scores;
    }
    const int context_size = static_cast<int>(llama_n_ctx(ctx_));
    if (static_cast<int>(tokens.size()) >= context_size) {
        tokens.erase(tokens.begin(),
                     tokens.end() - (context_size - std::min(256, context_size / 4)));
    }

    const int batch_size = std::max(1, static_cast<int>(llama_n_batch(ctx_)));
    for (std::size_t offset = 0; offset < tokens.size();
         offset += static_cast<std::size_t>(batch_size)) {
        if (cancel && cancel()) {
            return scores;
        }
        const int count = static_cast<int>(
            std::min<std::size_t>(static_cast<std::size_t>(batch_size), tokens.size() - offset));
        llama_batch batch = llama_batch_get_one(tokens.data() + offset, count);
        if (llama_decode(ctx_, batch) != 0) {
            return scores;
        }
    }

    const int n_vocab   = llama_vocab_n_tokens(vocab);
    const auto n_prompt = static_cast<llama_pos>(tokens.size());

    // Keep a copy. Scoring a label of more than one token decodes into the
    // context, which replaces the logits this needs for the label after it.
    const float* raw = llama_get_logits_ith(ctx_, -1);
    if (raw == nullptr || n_vocab <= 0) {
        return scores;
    }
    const std::vector<float> after_prompt(raw, raw + n_vocab);

    // log softmax at one position, for one token.
    const auto log_probability = [n_vocab](const float* logits, llama_token token) {
        const float highest = *std::max_element(logits, logits + n_vocab);
        float sum = 0.0F;
        for (int i = 0; i < n_vocab; ++i) {
            sum += std::exp(logits[i] - highest);
        }
        return logits[token] - highest - std::log(sum);
    };

    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (cancel && cancel()) {
            break;
        }
        std::vector<llama_token> label = tokenize(vocab, labels[i], false);
        if (label.empty()) {
            continue;
        }

        const float* logits = after_prompt.data();
        float        total  = 0.0F;
        bool         decoded = false;
        for (std::size_t t = 0; t < label.size(); ++t) {
            total += log_probability(logits, label[t]);
            if (t + 1 == label.size()) {
                break;  // nothing after it to score, so nothing to decode
            }
            llama_batch batch = llama_batch_get_one(&label[t], 1);
            if (llama_decode(ctx_, batch) != 0) {
                total = kUnscored;
                break;
            }
            decoded = true;
            logits  = llama_get_logits_ith(ctx_, -1);
            if (logits == nullptr) {
                total = kUnscored;
                break;
            }
        }
        scores[i] = total;

        // Put the context back to the end of the prompt so the next label is
        // scored against the same state rather than against this one.
        if (decoded) {
            llama_memory_seq_rm(memory, 0, n_prompt, -1);
        }
    }
    return scores;
}

GenerationStats LoadedModel::generate(const std::string& prompt,
                                      const ModelParams& params,
                                      const TokenCallback& on_token,
                                      const CancelCallback& cancel,
                                      const std::string& grammar) {
    GenerationStats stats;
    const llama_vocab* vocab = llama_model_get_vocab(model_);

    std::vector<llama_token> tokens = tokenize(vocab, prompt, true);
    if (tokens.empty()) {
        llama_memory_clear(llama_get_memory(ctx_), true);
        cached_.clear();
        return stats;
    }

    const int context_size = static_cast<int>(llama_n_ctx(ctx_));
    if (static_cast<int>(tokens.size()) >= context_size) {
        // Keep the tail: the newest turn matters more than the oldest.
        tokens.erase(tokens.begin(),
                     tokens.end() - (context_size - std::min(256, context_size / 4)));
    }
    stats.prompt_tokens = static_cast<int>(tokens.size());

    // Everything the context already holds of this prompt is kept. A
    // conversation resends its whole history each turn and all but the newest
    // exchange of it is identical, so on a large expert this is most of the
    // wait before the first token. See LoadedModel::cached_.
    const std::size_t reused = reuse_prefix(tokens);
    stats.prompt_reused = static_cast<int>(reused);

    llama_sampler* chain = llm::build_sampler_chain(vocab, params, grammar);

    // Guarded immediately: the chain must be freed on every path out of this
    // function, including the early returns below.
    struct ChainGuard {
        llama_sampler* chain;
        ~ChainGuard() { llama_sampler_free(chain); }
    } guard{chain};

    // --- prompt ingestion --------------------------------------------------
    const auto prompt_start = Clock::now();
    const int batch_size = std::max(1, params.n_batch);
    for (std::size_t offset = reused; offset < tokens.size();
         offset += static_cast<std::size_t>(batch_size)) {
        if (cancel && cancel()) {
            stats.canceled = true;
            stats.prompt_ms = ms_since(prompt_start);
            cached_.clear();  // the cache holds part of a prompt nobody will finish
            return stats;
        }
        const int count = static_cast<int>(
            std::min<std::size_t>(static_cast<std::size_t>(batch_size), tokens.size() - offset));
        llama_batch batch = llama_batch_get_one(tokens.data() + offset, count);
        if (llama_decode(ctx_, batch) != 0) {
            stats.prompt_ms = ms_since(prompt_start);
            cached_.clear();
            return stats;
        }
        cached_.insert(cached_.end(), tokens.begin() + static_cast<long>(offset),
                       tokens.begin() + static_cast<long>(offset) + count);
    }
    stats.prompt_ms = ms_since(prompt_start);

    // --- token generation --------------------------------------------------
    const auto output_start = Clock::now();
    const int limit = params.max_tokens > 0 ? params.max_tokens : context_size;
    std::string pending;  // holds bytes of an unfinished UTF-8 sequence

    for (int produced = 0; produced < limit; ++produced) {
        if (cancel && cancel()) {
            stats.canceled = true;
            break;
        }

        // llama_sampler_sample() accepts the token into the chain itself, so
        // there is no llama_sampler_accept() call here on purpose: a second
        // accept advances the grammar twice and empties its stack, which
        // surfaces as "Unexpected empty grammar stack after accepting piece".
        const llama_token token = llama_sampler_sample(chain, ctx_, -1);
        if (llama_vocab_is_eog(vocab, token)) {
            break;
        }

        pending += piece_for(vocab, token);
        if (std::string ready = detail::take_complete_utf8(pending);
            !ready.empty() && on_token) {
            on_token(ready);
        }

        ++stats.output_tokens;
        if (produced + 1 >= limit) {
            stats.hit_limit = true;
        }

        llama_token next = token;
        llama_batch batch = llama_batch_get_one(&next, 1);
        if (llama_decode(ctx_, batch) != 0) {
            cached_.clear();
            break;
        }
        cached_.push_back(next);
    }

    // Flush whatever is left, even if it is an incomplete sequence -- the model
    // is finished, so no more bytes are coming.
    if (!pending.empty() && on_token) {
        on_token(pending);
    }
    stats.output_ms = ms_since(output_start);
    return stats;
}


}  // namespace crucible
