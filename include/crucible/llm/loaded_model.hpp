// SPDX-License-Identifier: MIT
//
// One model file, loaded and ready to generate from.
//
// Freeing a LoadedModel releases the weights, which is precisely what an expert
// swap does -- so the lifetime of this object *is* the JIT loading strategy.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "crucible/config/config.hpp"

// llama.cpp's token type, spelled out so this header does not drag llama.h in.
using llama_token = std::int32_t;

struct llama_model;
struct llama_context;

namespace crucible {

/// One turn of a conversation, as handed to the model's chat template.
struct ChatMessage {
    std::string role;     ///< "system" | "user" | "assistant"
    std::string content;
};

/// Reported after a generation finishes, for the status line.
struct GenerationStats {
    int    prompt_tokens = 0;
    /// How many of `prompt_tokens` the context already held, and so did not
    /// have to read again. See LoadedModel::cached_.
    int    prompt_reused = 0;
    int    output_tokens = 0;
    double prompt_ms     = 0.0;
    double output_ms     = 0.0;
    bool   canceled     = false;
    /// Stopped at max_tokens rather than at an end-of-turn token.
    bool   hit_limit     = false;

    double tokens_per_second() const;
};

/// Called for each chunk of decoded text. Chunks are always complete UTF-8, so
/// the UI can append them straight to a string without splitting a codepoint.
using TokenCallback = std::function<void(std::string_view)>;

/// Return true to abort. Polled between tokens and during model loading.
using CancelCallback = std::function<bool()>;

/// Load progress in [0, 1].
using ProgressCallback = std::function<void(float)>;

namespace detail {

/// How much of `wanted` a context holding `cached` does not have to read again.
///
/// The common prefix, with one exception: never all of it. llama_decode has to
/// be given something, and the logits it leaves behind are what the first token
/// is sampled from -- so reusing an entire prompt would leave the model with
/// nothing to say. Resending the same prompt twice is not a strange thing to do
/// (it is what pressing enter on an unchanged line does), so the rule is
/// enforced here rather than left to the caller to remember.
std::size_t reusable_prefix(const std::vector<llama_token>& cached,
                            const std::vector<llama_token>& wanted);

}  // namespace detail

/// What LoadedModel::score_labels reports for a label it could not evaluate --
/// a decode that failed, or a canceled call. Far below any real
/// log-probability, so it never wins a comparison by accident.
inline constexpr float kUnscored = -1e30F;

class LoadedModel {
public:
    ~LoadedModel();
    LoadedModel(const LoadedModel&)            = delete;
    LoadedModel& operator=(const LoadedModel&) = delete;

    /// Render `messages` through the model's own chat template, falling back to
    /// ChatML for GGUFs that ship without one.
    std::string format_chat(const std::vector<ChatMessage>& messages,
                            bool add_assistant_prefix) const;

    /// Generate from `prompt`, streaming complete UTF-8 chunks to `on_token`.
    ///
    /// `grammar` (GBNF) is optional; when given, the sampler cannot leave it.
    /// That is what makes the delegator incapable of naming a subject that does
    /// not exist -- an invalid route is unrepresentable, not merely unlikely.
    GenerationStats generate(const std::string& prompt,
                             const ModelParams& params,
                             const TokenCallback& on_token,
                             const CancelCallback& cancel,
                             const std::string& grammar = {});

    /// How likely the model finds each of `labels` as the continuation of
    /// `prompt`, as a natural log-probability.
    ///
    /// This is classification rather than generation, and the difference
    /// matters. Generating under a grammar picks whichever allowed token scores
    /// highest at each step and never compares the choices as wholes -- so a
    /// label the model was only marginally keenest on wins outright, and there
    /// is no number afterwards to say how close it was. Scoring evaluates the
    /// prompt once and then measures every label against that same state, which
    /// costs one forward pass plus a token or two per label, and returns
    /// something a confidence threshold can actually be applied to.
    ///
    /// The score is the sum over the label's tokens, which is the probability
    /// of the sequence. A label that tokenizes longer is very slightly
    /// penalised; with labels of two or three tokens the effect is far smaller
    /// than the differences being measured.
    std::vector<float> score_labels(const std::string& prompt,
                                    const std::vector<std::string>& labels,
                                    const CancelCallback& cancel);

    /// How many tokens `text` becomes for this model.
    ///
    /// The only honest way to ask "will this conversation fit". A character
    /// count is out by a factor of three or four depending on the tokenizer and
    /// the language, and the answer decides whether the oldest exchange gets
    /// thrown away -- which is not a thing to get wrong by a factor of three.
    int count_tokens(const std::string& text) const;

    /// The context this model was loaded with, in tokens.
    int context_size() const;

    const std::string& path() const { return path_; }
    std::uint64_t      params() const;       ///< parameter count, for the UI
    std::uint64_t      bytes() const;        ///< in-memory size

private:
    friend class ModelHost;
    LoadedModel(llama_model* model, llama_context* ctx, std::string path);

    /// Reuse whatever of `tokens` the context already holds, and return how
    /// many tokens that turned out to be. Leaves the cache holding exactly that
    /// prefix, so the caller decodes from there.
    std::size_t reuse_prefix(const std::vector<llama_token>& tokens);

    llama_model*   model_ = nullptr;
    llama_context* ctx_   = nullptr;
    std::string    path_;

    /// The tokens currently in the context's KV cache, in order.
    ///
    /// A conversation re-sends its whole history every turn, and all but the
    /// last exchange of it is identical to what the cache already holds. On a
    /// 32 GB expert, re-reading four thousand tokens of that costs about five
    /// seconds before the first new token appears -- every turn, growing with
    /// the conversation. Keeping the token ids is what makes it possible to
    /// find the common prefix and skip it.
    ///
    /// Only ever the truth: every path that leaves the cache in an unknown
    /// state clears this, and the next turn starts from cold rather than from a
    /// guess about what survived.
    std::vector<llama_token> cached_;
};

}  // namespace crucible
