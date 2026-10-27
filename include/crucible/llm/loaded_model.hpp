// SPDX-License-Identifier: MIT
//
// One model file, loaded and ready to generate from.
//
// Freeing a LoadedModel releases the weights, which is precisely what an expert
// swap does -- so the lifetime of this object *is* the JIT loading strategy.
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "crucible/config/config.hpp"
#include "crucible/llm/chat_model.hpp"

// llama.cpp's token type, spelled out so this header does not drag llama.h in.
using llama_token = std::int32_t;

struct llama_model;
struct llama_context;
struct mtmd_context;

namespace crucible {

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

/// Whether a cache whose oldest position is `oldest` still holds what a token
/// read at `position` looks back at, through a sliding window of `window`
/// tokens. Always with no window; never with nothing held (-1).
///
/// A window cache keeps only the last window of a long sequence, so rewinding
/// one into the middle and reading on from there is only right while the
/// window before that point is still in it. Asked one position further back
/// than the window reaches, which also covers a chunked window: that reaches
/// back to where its chunk began, never further.
bool holds_window(std::int32_t oldest, std::size_t position, std::int32_t window);

}  // namespace detail

/// What LoadedModel::score_labels reports for a label it could not evaluate --
/// a decode that failed, or a canceled call. Far below any real
/// log-probability, so it never wins a comparison by accident.
inline constexpr float kUnscored = -1e30F;

class LoadedModel : public ChatModel {
public:
    ~LoadedModel() override;
    LoadedModel(const LoadedModel&)            = delete;
    LoadedModel& operator=(const LoadedModel&) = delete;

    // --- ChatModel --------------------------------------------------------

    /// Render the conversation through the model's own template and generate
    /// from it. The whole of what answering means for a model that is here.
    ChatResult chat(const ChatRequest& request, const ChatSink& sink) override;

    /// Measured through this model's tokenizer and its chat template, which is
    /// the only figure that means anything: the template adds role markers and
    /// turn separators, and on a long conversation those are hundreds of
    /// tokens.
    int prompt_tokens(const std::vector<ChatMessage>& messages) const override;

    bool reasons_inline() const override { return true; }

    /// gpt-oss's: its chat template is the harmony format, which reads
    /// `Reasoning: <level>` from the system message. See
    /// Config::reasoning_effort.
    bool takes_effort() const override;

    /// When a projector was found beside the model and it reads pictures --
    /// a vision model, Qwen2.5-VL or Gemma 3 with its mmproj file. See
    /// chat_with_pictures.
    bool sees_images() const override;

    /// The projector this model reads pictures through, or empty.
    const std::string& projector() const { return projector_; }

    // --- and what only a local model can do --------------------------------

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
    /// costs one forward pass for the prompt and one for the labels' later
    /// tokens together, and returns something a confidence threshold can
    /// actually be applied to.
    ///
    /// The score is the sum over the label's tokens, which is the probability
    /// of the sequence. A label that tokenizes longer is very slightly
    /// penalised; with labels of two or three tokens the effect is far smaller
    /// than the differences being measured.
    ///
    /// `prefix` is the part of `prompt` that recurs from call to call -- a
    /// delegator's instructions and worked examples, the same for every
    /// question. Its cache is kept, and the next prompt starting with it reads
    /// only what follows: one question rather than the whole roster again.
    std::vector<float> score_labels(const std::string& prompt,
                                    const std::vector<std::string>& labels,
                                    const CancelCallback& cancel,
                                    const std::string& prefix = {});

    /// How many tokens `text` becomes for this model.
    ///
    /// The only honest way to ask "will this conversation fit". A character
    /// count is out by a factor of three or four depending on the tokenizer and
    /// the language, and the answer decides whether the oldest exchange gets
    /// thrown away -- which is not a thing to get wrong by a factor of three.
    int count_tokens(const std::string& text) const;

    /// The context this model was loaded with, in tokens.
    int context_size() const override;

    const std::string& path() const { return path_; }
    std::uint64_t      params() const;       ///< parameter count, for the UI
    std::uint64_t      bytes() const;        ///< in-memory size

private:
    friend class ModelHost;
    LoadedModel(llama_model* model, llama_context* ctx, std::string path);

    /// A turn with pictures in it, for a model with a projector: the
    /// conversation rendered with a marker where each picture goes, the
    /// pictures decoded and read through the projector into the context, and
    /// the answer sampled from there. The context starts over for it --
    /// what it held was text, and a picture's tokens are not text tokens to
    /// compare against.
    ChatResult chat_with_pictures(const ChatRequest& request, const ChatSink& sink);

    /// The answer, sampled from whatever the context holds now: the second
    /// half of generate(), shared with chat_with_pictures.
    void sample(const ModelParams& params, const TokenCallback& on_token, const CancelCallback& cancel,
                const std::string& grammar, GenerationStats& stats);

    /// Reuse whatever of `tokens` the context already holds, and return how
    /// many tokens that turned out to be. Leaves the cache holding exactly that
    /// prefix, so the caller decodes from there.
    std::size_t reuse_prefix(const std::vector<llama_token>& tokens);

    /// Whether the cache still holds what a token read again at `position`
    /// looks back at. Always, unless the model has sliding-window layers: a
    /// window cache keeps only the last window of a long sequence, and the
    /// positions before it are gone.
    bool holds_window_before(std::size_t position) const;

    /// Take checkpoint_ where the cache stands now, when it is one a cut
    /// cannot rewind: a window cache, or a recurrent model's state.
    void keep_checkpoint();

    /// Read `tokens` from `from` to `to` into the cache, a batch at a time.
    /// False when a decode failed or `cancel` said stop.
    bool read(const std::vector<llama_token>& tokens, std::size_t from, std::size_t to,
              const CancelCallback& cancel);

    /// Put the first `count` of `tokens` in the cache: from a kept prompt
    /// when one matches, read and kept for next time when none does. False
    /// when the reading failed or was stopped.
    bool recall(const std::vector<llama_token>& tokens, std::size_t count, const CancelCallback& cancel);

    /// Add to `scores` what every label's tokens after its first score, read
    /// after the `n_prompt` tokens of prompt the cache holds. False when a
    /// label could not be read, or `cancel` said stop.
    bool score_rest(const std::vector<std::vector<llama_token>>& labels, std::size_t n_prompt,
                    std::vector<float>& scores, const CancelCallback& cancel);

    /// The cache's bookkeeping is no longer the truth: cached_ and
    /// checkpoint_ go, and the next prompt starts from cold.
    void forget();

    llama_model*   model_ = nullptr;
    llama_context* ctx_   = nullptr;
    std::string    path_;

    /// Loaded with the window cache. See ModelParams::window_cache.
    bool window_cache_ = false;

    /// What it holds on each card, by ggml device index, as llama.cpp said
    /// when it was loaded: weights, cache and compute buffers. What the
    /// cards would have back were it let go. See ModelHost::could_hold.
    std::vector<std::uint64_t> on_cards_;

    /// A prompt prefix scoring has read, kept so the next prompt starting
    /// with it does not read it again. See score_labels.
    struct KeptPrompt {
        std::vector<llama_token>  tokens;
        std::vector<std::uint8_t> state;     ///< the cache holding exactly `tokens`
        std::uint64_t             used = 0;  ///< when it was last asked for
    };

    /// Six at most, the least recently used going first: the delegator
    /// shows its roster in three orders, and each is a prefix of its own,
    /// and decides whether a message is a build in two more (the
    /// orchestrator's intent.py). Five taking turns in four places would
    /// each be gone by the time it came round again. Each is no larger than
    /// the cache it came from.
    std::vector<KeptPrompt> kept_;
    std::uint64_t           kept_clock_ = 0;

    /// The projector, when the model has one: what turns a picture into
    /// tokens the model reads. Null for a model that reads text only.
    mtmd_context*  vision_ = nullptr;
    std::string    projector_;

    /// One caller at a time. A context is one conversation's worth of state
    /// on the card, and a build's agent and the chat may both be answered by
    /// this model: the second waits for the first's round rather than the
    /// two decoding into one cache. Recursive because chat() measures the
    /// prompt through prompt_tokens(), and that renders it through
    /// format_chat(), each of which also stands alone.
    mutable std::recursive_mutex use_;

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

    /// The part of the cache a cut cannot rewind -- a recurrent state, a
    /// sliding window -- as it stood after the first `tokens` of cached_: the
    /// end of the last prompt, which is where a conversation's next turn
    /// usually parts from the last one. A reasoning model's history drops the
    /// thinking it did there, and a window cache has long since let go of
    /// those positions by the time the reply is done. Empty when there is
    /// nothing a cut cannot put back.
    struct Checkpoint {
        std::size_t               tokens = 0;
        std::vector<std::uint8_t> state;
    };
    Checkpoint checkpoint_;
};

}  // namespace crucible
