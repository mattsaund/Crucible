// SPDX-License-Identifier: MIT
//
// A model that is somewhere else.
//
// An expert does not have to be a file on this machine. It can be Claude, or
// GPT, or Gemini, or whatever a hosting company is serving this month -- asked
// over HTTPS, answered a piece at a time, and held to the same contract a
// local model is: here is the conversation, tell me what you say.
//
// Two wire formats are spoken, because two is how many there are. Anthropic's
// Messages API is one. The other is the chat-completions shape OpenAI defined
// and everybody else copied, which is why one implementation reaches OpenAI,
// Gemini, DeepSeek, Kimi, Cloudflare, OpenRouter and a llama.cpp server on the
// next machine over.
//
// What this does *not* do is pretend the far end is uniform. Providers differ
// in which request fields they accept, and the differences are not published
// anywhere a program can read. So a request is sent in full, and when a
// provider answers 400 naming a field it will not take, that field is dropped
// and the request is sent again -- once, remembered for the session. See
// Quirks. The alternative is a table of model names that is out of date the
// week it is written.
//
// Everything here leaves the machine. That is the one thing about it that
// matters more than how it works, and it is why a provider has to be added by
// hand and a seat has to be pointed at it by hand: see config.hpp, Provider.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "crucible/config/config.hpp"
#include "crucible/llm/chat_model.hpp"

namespace crucible::remote {

/// What is known about one model at one provider. Asked for, not configured.
///
/// Only Anthropic publishes this in a form worth reading: its Models API says
/// how big the context is and which features a model takes. For everything
/// else these stay at "unknown" and the request is shaped by what the provider
/// turns out to accept.
struct ModelFacts {
    int  context_tokens    = 0;      ///< 0 when unknown
    int  max_output        = 0;      ///< 0 when unknown
    bool adaptive_thinking = false;  ///< takes thinking: {type: "adaptive"}
    bool effort            = false;  ///< takes output_config.effort
    bool server_fallback   = false;  ///< can re-run a declined request on another model
};

/// What a provider turned out not to accept.
///
/// Each one is set by a 400 that named the field, and each removes that field
/// from every later request to the same model. They only ever go from false to
/// true: a provider that rejected a field once is not asked again.
struct Quirks {
    // The chat-completions shape.
    bool no_sampling       = false;  ///< rejects temperature / top_p
    bool completion_tokens = false;  ///< wants max_completion_tokens, not max_tokens
    bool no_stream_usage   = false;  ///< rejects stream_options

    // The Messages shape, through a gateway that is not Anthropic's own.
    bool no_fallbacks     = false;
    bool no_cache_control = false;
    bool no_thinking      = false;
    bool no_effort        = false;
};

/// Hands out the model behind a seat, and stops whichever one is answering.
///
/// One per engine. It remembers what it has learned about each model for as
/// long as the providers stay the same, so the question "what can this model
/// do" is asked once rather than before every prompt.
class Hub {
public:
    Hub();
    ~Hub();
    Hub(const Hub&)            = delete;
    Hub& operator=(const Hub&) = delete;

    /// The model `params` names. Null, with `error` saying why, when the
    /// provider it names is not one that has been added.
    ///
    /// The pointer is good until the next call to `adopt`.
    ChatModel* model(const ModelParams& params, std::string& error);

    /// Take the provider list from `config`. What was learned about their
    /// models is kept unless the providers themselves changed.
    void adopt(const Config& config);

    /// Stop the request in flight, if there is one. Safe from any thread --
    /// which is the point: the thread reading the answer is blocked on the
    /// next line of it, and Stop is pressed somewhere else.
    void interrupt();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// The models `provider` offers, asked live and sorted.
///
/// For the picker. Empty with `error` set when the provider could not be
/// reached or would not say -- some do not list, and a model can still be
/// named by hand.
std::vector<std::string> list_models(const Provider& provider, std::string& error);

// --- exposed for the tests -------------------------------------------------
//
// Everything below is a pure function of its arguments. The part of this file
// that can be wrong without a network is exactly the part that turns a
// conversation into a request and a line of a stream back into text, and that
// is the part worth pinning down.

namespace detail {

/// What is sent when a reply may run "until the model stops" and the stream is
/// what keeps the connection from timing out. Capped by what the model itself
/// allows, when that is known.
inline constexpr int kUnboundedReply = 64000;

/// The context assumed for a model that will not say what it has. Large on
/// purpose: guessing small drops history that would have fit, and guessing
/// large costs one request the provider refuses in plain words.
inline constexpr int kAssumedContext = 128000;

/// The body of a Messages API request.
std::string anthropic_body(std::string_view model, const ChatRequest& request,
                           const ModelFacts& facts, const Quirks& quirks);

/// The body of a chat-completions request.
std::string openai_body(std::string_view model, const ChatRequest& request,
                        const Quirks& quirks);

/// One event out of a stream, reduced to what the caller does with it.
struct Event {
    std::string text;        ///< more of the answer
    std::string reasoning;   ///< more of the reasoning
    std::string stop;        ///< why it stopped, in the provider's own word
    std::string detail;      ///< what it said about stopping, when it said anything
    std::string error;       ///< the stream reported a failure
    std::string note;        ///< something to tell the reader; see ChatResult::notes
    int  input_tokens  = -1; ///< -1 when this event did not say
    int  output_tokens = -1;
    bool done          = false;
};

/// Read one `data:` payload of a Messages stream.
Event parse_anthropic(std::string_view data);

/// Read one `data:` payload of a chat-completions stream.
Event parse_openai(std::string_view data);

/// Learn from a refused request. True when `quirks` changed, which is the
/// signal that sending it again is worth doing.
bool adapt(Quirks& quirks, std::string_view kind, std::string_view message);

/// The sentence inside a provider's error payload. They do not agree on where
/// it goes, so this looks in every place one of them has put it.
std::string error_message(std::string_view body);

/// What Anthropic's Models API says about a model.
ModelFacts facts_from_model(std::string_view body);

/// The ids out of a model listing, either provider's.
std::vector<std::string> model_ids(std::string_view body);

/// A token count for a model whose tokenizer is somewhere else. High rather
/// than low; see ChatModel::prompt_tokens.
int estimate_tokens(const std::vector<ChatMessage>& messages);

}  // namespace detail

}  // namespace crucible::remote
