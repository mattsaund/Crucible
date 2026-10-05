// SPDX-License-Identifier: MIT
//
// Something that can be handed a conversation and will answer it.
//
// There are two kinds and the engine should not have to care which it has: a
// GGUF loaded onto the cards in this machine, and a model at a provider on the
// far side of an HTTPS request. Both take the same messages, both stream their
// answer back a piece at a time, both can be stopped mid-word. Everything that
// differs -- what "loading" means, how tokens are counted, where the reasoning
// comes out -- is behind this interface.
//
// The split is by what the engine asks rather than by what the models are. It
// asks three things of whoever has the turn: how much room is there, how much
// of it would this conversation take, and what do you say.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "crucible/config/config.hpp"

namespace crucible {

/// One turn of a conversation, as handed to a model.
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

/// What to answer, and how.
struct ChatRequest {
    const std::vector<ChatMessage>& messages;
    const ModelParams&              params;

    /// How hard a reasoning model should think: low | medium | high, or empty
    /// to leave it to the model.
    ///
    /// Here rather than in the system prompt because the two kinds of model
    /// want it in different places. A local one reads it as a line of the
    /// prompt, which the engine writes; a provider takes it as a field of the
    /// request, and a stray "Reasoning: high" in its system prompt is noise.
    std::string_view effort;
};

/// Where an answer goes as it arrives.
struct ChatSink {
    /// The answer. From a local model this may have the reasoning in it too,
    /// between markers -- see ChatModel::reasons_inline.
    TokenCallback on_text;

    /// Reasoning that arrived on a channel of its own, which is how a
    /// provider sends it.
    TokenCallback on_reasoning;

    CancelCallback cancel;
};

/// How a round of answering went.
struct ChatResult {
    GenerationStats stats;

    /// Why there is no answer, in words fit to put in the transcript. Empty
    /// when there is one. A canceled round is not an error: `stats.canceled`
    /// says so and this stays empty.
    std::string error;

    /// Things about this round worth saying above the reply -- that a
    /// different model answered than the one that was asked, for instance.
    std::vector<std::string> notes;

    /// Set with `error` when whatever had streamed before the failure is not
    /// an answer and must not be left on screen as one. A model that declines
    /// partway through has not half-answered.
    bool discard_partial = false;
};

class ChatModel {
public:
    virtual ~ChatModel() = default;

    /// Answer `request`, streaming to `sink` as it goes.
    virtual ChatResult chat(const ChatRequest& request, const ChatSink& sink) = 0;

    /// How many tokens `messages` would occupy.
    ///
    /// Exact for a local model, which has its own tokenizer to ask. An
    /// estimate for a remote one, erring high: the figure decides whether old
    /// exchanges are dropped, and dropping one too early costs less than a
    /// request refused for being too long.
    virtual int prompt_tokens(const std::vector<ChatMessage>& messages) const = 0;

    /// The context this model has, in tokens.
    virtual int context_size() const = 0;

    /// Whether the text handed to `on_text` may carry the model's reasoning
    /// inline, to be split out by a ResponseFilter. True for a local model,
    /// and for an open model served by somebody else, which writes the same
    /// markers there as here. False for Claude, which sends its reasoning
    /// separately -- and whose answer may legitimately contain the very text
    /// the filter hunts for.
    virtual bool reasons_inline() const = 0;
};

}  // namespace crucible
