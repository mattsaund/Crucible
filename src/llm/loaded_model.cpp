// SPDX-License-Identifier: MIT
//
// Generating text from a loaded model.
//
// The loop is deliberately plain: feed the prompt, then sample a token at a
// time. What the KV cache already holds of the prompt is kept rather than fed
// again -- a conversation resends its whole history each turn, and all but the
// newest exchange of it is already there. See reuse_prefix, and
// LoadedModel::cached_ for why that bookkeeping is never a guess.
#include "crucible/llm/loaded_model.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <vector>

#include <llama.h>

#if defined(CRUCIBLE_HAS_MTMD)
#include <mtmd-helper.h>
#include <mtmd.h>
#endif

#include "crucible/llm/sampling.hpp"
#include "crucible/util/format.hpp"
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

/// How many prompt prefixes scoring keeps. See LoadedModel::kept_.
constexpr std::size_t kKeptPrompts = 6;

/// What a logit at one position is less, to be a natural log-probability
/// there: the log of the softmax's denominator. Once per position, however
/// many labels are scored at it -- a pass over the whole vocabulary, 201,088
/// entries on gpt-oss.
float log_normalizer(const float* logits, int n_vocab) {
    const float highest = *std::max_element(logits, logits + n_vocab);
    float sum = 0.0F;
    for (int i = 0; i < n_vocab; ++i) {
        sum += std::exp(logits[i] - highest);
    }
    return highest + std::log(sum);
}

/// The cache as it stands, for putting back later: the whole of it with
/// `flags` 0, and with LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY only what a cut
/// cannot rewind -- a recurrent state, a sliding window -- which is empty of
/// everything but a header for a model with neither. Empty when llama.cpp
/// could not give it.
std::vector<std::uint8_t> state_of(llama_context* ctx, llama_state_seq_flags flags) {
    std::vector<std::uint8_t> state(llama_state_seq_get_size_ext(ctx, 0, flags));
    if (state.empty() || llama_state_seq_get_data_ext(ctx, state.data(), state.size(), 0, flags) != state.size()) {
        return {};
    }
    return state;
}

/// Put back what state_of took. False when it did not all go back, which
/// leaves the part it was putting back empty.
bool put_back(llama_context* ctx, const std::vector<std::uint8_t>& state, llama_state_seq_flags flags) {
    return !state.empty()
        && llama_state_seq_set_data_ext(ctx, state.data(), state.size(), 0, flags) == state.size();
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
#if defined(CRUCIBLE_HAS_MTMD)
    if (vision_ != nullptr) {
        mtmd_free(vision_);
    }
#endif
    if (ctx_ != nullptr) {
        llama_free(ctx_);
    }
    if (model_ != nullptr) {
        llama_model_free(model_);
    }
}

int LoadedModel::count_tokens(const std::string& text) const {
    const std::lock_guard<std::recursive_mutex> lock(use_);
    const llama_vocab* vocab = llama_model_get_vocab(model_);
    if (vocab == nullptr || text.empty()) {
        return 0;
    }
    return static_cast<int>(tokenize(vocab, text, false).size());
}

bool LoadedModel::sees_images() const {
#if defined(CRUCIBLE_HAS_MTMD)
    return vision_ != nullptr && mtmd_support_vision(vision_);
#else
    return false;
#endif
}

ChatResult LoadedModel::chat(const ChatRequest& request, const ChatSink& sink) {
    const std::lock_guard<std::recursive_mutex> lock(use_);
    if (sees_images()
        && std::any_of(request.messages.begin(), request.messages.end(),
                       [](const ChatMessage& m) { return !m.images.empty(); })) {
        return chat_with_pictures(request, sink);
    }
    ChatResult result;
    result.stats = generate(format_chat(request.messages, /*add_assistant_prefix=*/true),
                            request.params, sink.on_text, sink.cancel);
    return result;
}

int LoadedModel::prompt_tokens(const std::vector<ChatMessage>& messages) const {
    const std::lock_guard<std::recursive_mutex> lock(use_);
    return count_tokens(format_chat(messages, /*add_assistant_prefix=*/true));
}

int LoadedModel::context_size() const {
    return static_cast<int>(llama_n_ctx(ctx_));
}

std::uint64_t LoadedModel::params() const { return llama_model_n_params(model_); }
std::uint64_t LoadedModel::bytes()  const { return llama_model_size(model_); }

bool LoadedModel::takes_effort() const {
    const char* tmpl = llama_model_chat_template(model_, nullptr);
    if (tmpl == nullptr) {
        return false;
    }
    const std::string_view text(tmpl);
    return text.find("reasoning_effort") != std::string_view::npos
        || text.find("<|channel|>") != std::string_view::npos;
}

std::string LoadedModel::format_chat(const std::vector<ChatMessage>& messages,
                                     bool add_assistant_prefix) const {
    const std::lock_guard<std::recursive_mutex> lock(use_);
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

bool holds_window(std::int32_t oldest, std::size_t position, std::int32_t window) {
    if (window <= 0) {
        return true;
    }
    const auto at = static_cast<std::int64_t>(position);
    return oldest >= 0 && oldest <= std::max<std::int64_t>(0, at - window);
}

}  // namespace detail

std::size_t LoadedModel::reuse_prefix(const std::vector<llama_token>& tokens) {
    llama_memory_t memory = llama_get_memory(ctx_);

    const std::size_t common = detail::reusable_prefix(cached_, tokens);
    if (common > 0 && common == cached_.size()) {
        // Nothing to remove when the new prompt simply continues the old one,
        // which is what a conversation growing by a turn looks like. Even a
        // window cache can go on from its own end: it always keeps the window
        // before it.
        return common;
    }
    if (common > 0) {
        // Otherwise the tail has to go, and that can fail. A recurrent model
        // keeps a state rather than a per-token cache, and a state cannot be
        // rewound to an arbitrary point: llama.cpp says so by returning false.
        // A window cache fails without saying so -- the cut succeeds, and the
        // first token read again looks back at positions that are gone -- so
        // it is asked first whether it still holds them.
        const auto at = static_cast<llama_pos>(common);
        if (holds_window_before(common) && llama_memory_seq_rm(memory, 0, at, -1)) {
            cached_.resize(common);
            if (checkpoint_.tokens > common) {
                checkpoint_ = {};
            }
            return common;
        }
        // Where a cut alone cannot do it, the checkpoint puts back what the
        // cut cannot, as it stood at the end of the last prompt, and only what
        // came after that is read again.
        if (checkpoint_.tokens > 0 && checkpoint_.tokens <= common
            && put_back(ctx_, checkpoint_.state, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY)
            && llama_memory_seq_rm(memory, 0, static_cast<llama_pos>(checkpoint_.tokens), -1)) {
            cached_.resize(checkpoint_.tokens);
            return checkpoint_.tokens;
        }
    }

    // And the honest answer to a cache that cannot go back is to start again
    // from cold.
    llama_memory_clear(memory, true);
    forget();
    return 0;
}

bool LoadedModel::holds_window_before(std::size_t position) const {
    const llama_pos window = llama_model_n_swa(model_);
    return window <= 0
        || detail::holds_window(llama_memory_seq_pos_min(llama_get_memory(ctx_), 0), position, window);
}

void LoadedModel::keep_checkpoint() {
    checkpoint_ = {};
    if (!window_cache_ && !llama_model_is_recurrent(model_) && !llama_model_is_hybrid(model_)) {
        return;   // a cut rewinds the whole cache to anywhere
    }
    std::vector<std::uint8_t> state = state_of(ctx_, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
    if (!state.empty()) {
        checkpoint_.tokens = cached_.size();
        checkpoint_.state  = std::move(state);
    }
}

void LoadedModel::forget() {
    cached_.clear();
    checkpoint_ = {};
}

bool LoadedModel::read(const std::vector<llama_token>& tokens, std::size_t from, std::size_t to,
                       const CancelCallback& cancel) {
    const int batch_size = std::max(1, static_cast<int>(llama_n_batch(ctx_)));
    for (std::size_t offset = from; offset < to; offset += static_cast<std::size_t>(batch_size)) {
        if (cancel && cancel()) {
            return false;
        }
        const int count = static_cast<int>(
            std::min<std::size_t>(static_cast<std::size_t>(batch_size), to - offset));
        // llama_batch_get_one only reads through the pointer it is given.
        llama_batch batch = llama_batch_get_one(const_cast<llama_token*>(tokens.data()) + offset, count);
        if (llama_decode(ctx_, batch) != 0) {
            return false;
        }
    }
    return true;
}

bool LoadedModel::recall(const std::vector<llama_token>& tokens, std::size_t count,
                         const CancelCallback& cancel) {
    const auto same = [&](const KeptPrompt& kept) {
        return kept.tokens.size() == count && std::equal(kept.tokens.begin(), kept.tokens.end(), tokens.begin());
    };
    if (const auto found = std::find_if(kept_.begin(), kept_.end(), same); found != kept_.end()) {
        if (put_back(ctx_, found->state, 0)) {
            found->used = ++kept_clock_;
            return true;
        }
        kept_.erase(found);   // it no longer fits this context, so it is read again
    }

    llama_memory_clear(llama_get_memory(ctx_), true);
    if (!read(tokens, 0, count, cancel)) {
        return false;
    }
    std::vector<std::uint8_t> state = state_of(ctx_, 0);
    if (!state.empty()) {
        if (kept_.size() >= kKeptPrompts) {
            kept_.erase(std::min_element(kept_.begin(), kept_.end(), [](const KeptPrompt& a, const KeptPrompt& b) {
                return a.used < b.used;
            }));
        }
        kept_.push_back(KeptPrompt{std::vector<llama_token>(tokens.begin(), tokens.begin() + static_cast<long>(count)),
                                   std::move(state), ++kept_clock_});
    }
    return true;
}

std::vector<float> LoadedModel::score_labels(const std::string& prompt,
                                             const std::vector<std::string>& labels,
                                             const CancelCallback& cancel,
                                             const std::string& prefix) {
    const std::lock_guard<std::recursive_mutex> lock(use_);
    std::vector<float> scores(labels.size(), kUnscored);
    if (labels.empty()) {
        return scores;
    }

    const llama_vocab* vocab = llama_model_get_vocab(model_);
    llama_memory_t     memory = llama_get_memory(ctx_);
    // Scoring leaves the cache holding labels rather than a conversation, so
    // the prefix bookkeeping is not valid across it either way.
    forget();

    std::vector<llama_token> tokens = tokenize(vocab, prompt, true);
    if (tokens.empty()) {
        llama_memory_clear(memory, true);
        return scores;
    }
    const int   context_size = static_cast<int>(llama_n_ctx(ctx_));
    std::size_t recurs       = 0;
    if (static_cast<int>(tokens.size()) >= context_size) {
        // Its head goes, and the prefix with it.
        tokens.erase(tokens.begin(),
                     tokens.end() - (context_size - std::min(256, context_size / 4)));
    } else if (!prefix.empty() && prompt.compare(0, prefix.size(), prefix) == 0) {
        // The prefix as it tokenizes inside the whole prompt: a token can
        // straddle where it ends, and only what the two agree on is the same
        // from one call to the next. Never all of the prompt, which leaves a
        // token to read for the logits to come from.
        recurs = detail::reusable_prefix(tokenize(vocab, prefix, true), tokens);
    }

    // What recurs, from where it was kept; the rest, read now.
    if (recurs == 0) {
        llama_memory_clear(memory, true);
    } else if (!recall(tokens, recurs, cancel)) {
        return scores;
    }
    if (!read(tokens, recurs, tokens.size(), cancel)) {
        return scores;
    }

    const int n_vocab = llama_vocab_n_tokens(vocab);
    const float* raw  = llama_get_logits_ith(ctx_, -1);
    if (raw == nullptr || n_vocab <= 0) {
        return scores;
    }

    // Every label's first token, from what the prompt left; the rest of each
    // label is read after the prompt and scored where it was read. A label
    // that cannot be read leaves no score worth having for the others, and a
    // route on some of its labels is a route on a guess: none, then.
    const float normalizer = log_normalizer(raw, n_vocab);
    std::vector<std::vector<llama_token>> tokenized(labels.size());
    for (std::size_t i = 0; i < labels.size(); ++i) {
        tokenized[i] = tokenize(vocab, labels[i], false);
        if (!tokenized[i].empty()) {
            scores[i] = raw[tokenized[i][0]] - normalizer;
        }
    }
    if (!score_rest(tokenized, tokens.size(), scores, cancel)) {
        return std::vector<float>(labels.size(), kUnscored);
    }
    return scores;
}

bool LoadedModel::score_rest(const std::vector<std::vector<llama_token>>& labels, std::size_t n_prompt,
                             std::vector<float>& scores, const CancelCallback& cancel) {
    llama_memory_t memory  = llama_get_memory(ctx_);
    const int      n_vocab = llama_vocab_n_tokens(llama_model_get_vocab(model_));
    const auto     end     = static_cast<llama_pos>(n_prompt);

    std::vector<std::size_t> longer;   // the labels of more than one token
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (labels[i].size() > 1) {
            longer.push_back(i);
        }
    }

    const auto branches = static_cast<std::size_t>(std::max(1U, llama_n_seq_max(ctx_)) - 1);
    if (branches > 0) {
        // Side by side, each label on a sequence of its own branched from the
        // prompt's, as many at once as the context has sequences for: one
        // decode for a roster, where one label after another took one per
        // token -- eight an order on gpt-oss, which was most of a route once
        // the prompt was kept. The prompt's own sequence is never touched, so
        // nothing has to be put back after.
        const int batch_size = std::max(1, static_cast<int>(llama_n_batch(ctx_)));
        std::size_t next = 0;
        while (next < longer.size()) {
            if (cancel && cancel()) {
                return false;   // nothing half scored is handed back
            }
            std::vector<std::size_t> group;
            int count = 0;
            while (next < longer.size() && group.size() < branches
                   && count + static_cast<int>(labels[longer[next]].size()) - 1 <= batch_size) {
                count += static_cast<int>(labels[longer[next]].size()) - 1;
                group.push_back(longer[next++]);
            }
            if (group.empty()) {
                return false;   // a label longer than a batch
            }

            llama_batch batch = llama_batch_init(count, 0, 1);
            struct BatchGuard {
                llama_batch& batch;
                ~BatchGuard() { llama_batch_free(batch); }
            } batch_guard{batch};
            for (std::size_t g = 0; g < group.size(); ++g) {
                const auto branch = static_cast<llama_seq_id>(g + 1);
                llama_memory_seq_cp(memory, 0, branch, -1, -1);
                const std::vector<llama_token>& label = labels[group[g]];
                for (std::size_t t = 0; t + 1 < label.size(); ++t) {
                    const int at        = batch.n_tokens++;
                    batch.token[at]     = label[t];
                    batch.pos[at]       = end + static_cast<llama_pos>(t);
                    batch.n_seq_id[at]  = 1;
                    batch.seq_id[at][0] = branch;
                    batch.logits[at]    = 1;
                }
            }
            bool read = llama_decode(ctx_, batch) == 0;
            int  at   = 0;
            for (std::size_t g = 0; g < group.size(); ++g) {
                const std::vector<llama_token>& label = labels[group[g]];
                for (std::size_t t = 0; read && t + 1 < label.size(); ++t, ++at) {
                    const float* logits = llama_get_logits_ith(ctx_, at);
                    read = logits != nullptr;
                    if (read) {
                        scores[group[g]] += logits[label[t + 1]] - log_normalizer(logits, n_vocab);
                    }
                }
                // A whole sequence always goes, recurrent state and all.
                llama_memory_seq_rm(memory, static_cast<llama_seq_id>(g + 1), -1, -1);
            }
            if (!read) {
                return false;
            }
        }
        return true;
    }

    // A context with the one sequence reads a label at a time on it, and cuts
    // it off again before the next. A cut is all an ordinary cache needs. A
    // recurrent state and a window cache hold what a cut cannot put back --
    // on LFM2 the cut fails outright, and each label's tokens used to stay in
    // the cache for the labels after it to be scored behind -- so those take
    // it from a copy of how the cache stood at the end of the prompt.
    const bool partial =
        window_cache_ || llama_model_is_recurrent(model_) || llama_model_is_hybrid(model_);
    const std::vector<std::uint8_t> at_prompt =
        partial && !longer.empty() ? state_of(ctx_, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY) : std::vector<std::uint8_t>();
    if (partial && !longer.empty() && at_prompt.empty()) {
        return false;
    }
    for (const std::size_t i : longer) {
        if (cancel && cancel()) {
            return false;
        }
        std::vector<llama_token> label = labels[i];
        for (std::size_t t = 0; t + 1 < label.size(); ++t) {
            llama_batch batch = llama_batch_get_one(&label[t], 1);
            const float* logits = llama_decode(ctx_, batch) == 0 ? llama_get_logits_ith(ctx_, -1) : nullptr;
            if (logits == nullptr) {
                return false;
            }
            scores[i] += logits[label[t + 1]] - log_normalizer(logits, n_vocab);
        }
        if (!((at_prompt.empty() || put_back(ctx_, at_prompt, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY))
              && llama_memory_seq_rm(memory, 0, end, -1))) {
            return false;
        }
    }
    return true;
}

GenerationStats LoadedModel::generate(const std::string& prompt,
                                      const ModelParams& params,
                                      const TokenCallback& on_token,
                                      const CancelCallback& cancel,
                                      const std::string& grammar) {
    const std::lock_guard<std::recursive_mutex> lock(use_);
    GenerationStats stats;
    const llama_vocab* vocab = llama_model_get_vocab(model_);

    std::vector<llama_token> tokens = tokenize(vocab, prompt, true);
    if (tokens.empty()) {
        llama_memory_clear(llama_get_memory(ctx_), true);
        forget();
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

    // Stop asked inside a decode as well as between them. A batch of prompt is
    // one call, and on the processor one call of a large model's prompt is
    // seconds -- long enough that a Stop which only looks between batches
    // reads as a window that has stopped listening. The backends that run on
    // the processor poll this between the operations of a graph; on a card it
    // is the check between batches that does the work.
    struct AbortGuard {
        llama_context* ctx;
        ~AbortGuard() { llama_set_abort_callback(ctx, nullptr, nullptr); }
    } abort_guard{ctx_};
    if (cancel) {
        llama_set_abort_callback(
            ctx_,
            [](void* data) { return (*static_cast<const CancelCallback*>(data))(); },
            const_cast<CancelCallback*>(&cancel));
    }

    // --- prompt ingestion --------------------------------------------------
    const auto prompt_start = Clock::now();
    // No more than the context was made for. A model loaded before Batch was
    // raised keeps its old context, and a larger chunk than that is an assert
    // in llama_decode that ends the program.
    const int batch_size =
        std::max(1, std::min(params.n_batch, static_cast<int>(llama_n_batch(ctx_))));
    for (std::size_t offset = reused; offset < tokens.size();
         offset += static_cast<std::size_t>(batch_size)) {
        if (cancel && cancel()) {
            stats.canceled = true;
            stats.prompt_ms = ms_since(prompt_start);
            forget();  // the cache holds part of a prompt nobody will finish
            return stats;
        }
        const int count = static_cast<int>(
            std::min<std::size_t>(static_cast<std::size_t>(batch_size), tokens.size() - offset));
        llama_batch batch = llama_batch_get_one(tokens.data() + offset, count);
        if (llama_decode(ctx_, batch) != 0) {
            stats.canceled  = cancel && cancel();   // aborted from inside, by Stop
            stats.prompt_ms = ms_since(prompt_start);
            forget();
            return stats;
        }
        cached_.insert(cached_.end(), tokens.begin() + static_cast<long>(offset),
                       tokens.begin() + static_cast<long>(offset) + count);
    }
    keep_checkpoint();
    stats.prompt_ms = ms_since(prompt_start);

    sample(params, on_token, cancel, grammar, stats);
    return stats;
}

void LoadedModel::sample(const ModelParams& params, const TokenCallback& on_token, const CancelCallback& cancel,
                         const std::string& grammar, GenerationStats& stats) {
    const llama_vocab* vocab = llama_model_get_vocab(model_);
    const int context_size = static_cast<int>(llama_n_ctx(ctx_));
    llama_sampler* chain = llm::build_sampler_chain(vocab, params, grammar);
    struct ChainGuard {
        llama_sampler* chain;
        ~ChainGuard() { llama_sampler_free(chain); }
    } guard{chain};

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
            stats.canceled = cancel && cancel();
            forget();
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
}

ChatResult LoadedModel::chat_with_pictures(const ChatRequest& request, const ChatSink& sink) {
    ChatResult result;
#if defined(CRUCIBLE_HAS_MTMD)
    // A marker where each picture goes, ahead of the words it came with --
    // the order every vision model reads best -- and the pictures' bytes, in
    // the same order, for the projector.
    const std::string marker = mtmd_default_marker();
    std::vector<ChatMessage> marked = request.messages;
    std::vector<std::string> blobs;
    for (ChatMessage& message : marked) {
        std::string markers;
        for (const ChatImage& image : message.images) {
            std::string bytes;
            if (format::from_base64(image.data, bytes) && !bytes.empty()) {
                blobs.push_back(std::move(bytes));
                markers += marker + "\n";
            }
        }
        message.content = markers + message.content;
        message.images.clear();
    }
    const std::string prompt = format_chat(marked, /*add_assistant_prefix=*/true);

    std::vector<mtmd_bitmap*> bitmaps;
    struct Bitmaps {
        std::vector<mtmd_bitmap*>& all;
        ~Bitmaps() {
            for (mtmd_bitmap* one : all) {
                mtmd_bitmap_free(one);
            }
        }
    } free_bitmaps{bitmaps};
    const mtmd_helper_init_opt options = mtmd_helper_init_opt_default();
    for (const std::string& blob : blobs) {
        const mtmd_helper_bitmap_wrapper made = mtmd_helper_bitmap_init_from_buf(
            vision_, reinterpret_cast<const unsigned char*>(blob.data()), blob.size(), false, options);
        if (made.bitmap == nullptr) {
            result.error = "a picture could not be read: it is not a PNG, a JPEG or a GIF";
            return result;
        }
        bitmaps.push_back(made.bitmap);
    }

    mtmd_input_chunks* chunks = mtmd_input_chunks_init();
    struct Chunks {
        mtmd_input_chunks* chunks;
        ~Chunks() { mtmd_input_chunks_free(chunks); }
    } free_chunks{chunks};
    const mtmd_input_text text{prompt.c_str(), prompt.size(), /*add_special=*/true, /*parse_special=*/true};
    std::vector<const mtmd_bitmap*> given(bitmaps.begin(), bitmaps.end());
    if (mtmd_tokenize(vision_, chunks, &text, given.data(), given.size()) != 0) {
        result.error = "the pictures could not be read into the prompt";
        return result;
    }

    // Started over: what the context held was text, and is no prefix of this.
    llama_memory_clear(llama_get_memory(ctx_), true);
    forget();

    if (sink.cancel) {
        llama_set_abort_callback(
            ctx_, [](void* data) { return (*static_cast<const CancelCallback*>(data))(); },
            const_cast<CancelCallback*>(&sink.cancel));
    }
    struct AbortGuard {
        llama_context* ctx;
        ~AbortGuard() { llama_set_abort_callback(ctx, nullptr, nullptr); }
    } abort_guard{ctx_};

    const auto prompt_start = Clock::now();
    result.stats.prompt_tokens = static_cast<int>(mtmd_helper_get_n_tokens(chunks));
    llama_pos past = 0;
    const int batch_size =
        std::max(1, std::min(request.params.n_batch, static_cast<int>(llama_n_batch(ctx_))));
    if (mtmd_helper_eval_chunks(vision_, ctx_, chunks, 0, 0, batch_size,
                                /*logits_last=*/true, &past) != 0) {
        result.stats.canceled = sink.cancel && sink.cancel();
        if (!result.stats.canceled) {
            result.error = "the model could not read the pictures";
        }
        return result;
    }
    result.stats.prompt_ms = ms_since(prompt_start);
    sample(request.params, sink.on_text, sink.cancel, {}, result.stats);
    // What the context now holds starts with pictures, which no later prompt
    // of text can share: the next turn starts over too.
    forget();
#else
    (void)request;
    (void)sink;
    result.error = "this build of Crucible cannot read pictures with a local model";
#endif
    return result;
}


}  // namespace crucible
