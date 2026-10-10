// SPDX-License-Identifier: MIT
#include "crucible/llm/remote_model.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>

#include <nlohmann/json.hpp>

#include "crucible/util/format.hpp"
#include "crucible/util/http.hpp"

namespace crucible::remote {
namespace {

using json = nlohmann::json;
namespace http = util::http;

using Clock = std::chrono::steady_clock;

/// The version of the Messages API this file is written against. A date, sent
/// with every request; it changes when Anthropic changes the wire format, not
/// when a model is released.
constexpr const char* kAnthropicVersion = "2023-06-01";

/// The form of server-side fallback that names no substitute: a request the
/// model declines is re-run on whichever model Anthropic recommends for the
/// reason it was declined, inside the same call.
constexpr const char* kFallbackBeta = "server-side-fallback-2026-07-01";

/// The header under which a model's own entry lists what it may fall back to.
/// A different date from the one above, and that is not a typo: this one gates
/// the listing, that one gates the request field.
constexpr const char* kFallbackListingBeta = "server-side-fallback-2026-06-01";

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

/// `node[key]` when it is a string, and nothing otherwise. A provider's
/// payload is somebody else's document: every field may be missing, null, or
/// the wrong type, and none of those is worth an exception.
std::string string_at(const json& node, const char* key) {
    if (!node.is_object()) {
        return {};
    }
    const auto found = node.find(key);
    return found != node.end() && found->is_string() ? found->get<std::string>()
                                                     : std::string();
}

int int_at(const json& node, const char* key, int fallback) {
    if (!node.is_object()) {
        return fallback;
    }
    const auto found = node.find(key);
    return found != node.end() && found->is_number_integer() ? found->get<int>() : fallback;
}

const json& child(const json& node, const char* key) {
    static const json kNothing;
    if (!node.is_object()) {
        return kNothing;
    }
    const auto found = node.find(key);
    return found == node.end() ? kNothing : *found;
}

bool supported(const json& node) {
    const json& flag = child(node, "supported");
    return flag.is_boolean() && flag.get<bool>();
}

/// JSON text that cannot throw on the way out. A model's reply can hold a
/// byte sequence that is not UTF-8 -- a truncated character at a token
/// boundary is enough -- and nlohmann throws on those unless told to replace
/// them.
std::string dump(const json& document) {
    return document.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool is_effort(std::string_view effort) {
    return effort == "low" || effort == "medium" || effort == "high";
}

}  // namespace

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

namespace detail {

namespace {

/// A message's text, with a line saying its pictures were left out when the
/// provider has refused pictures -- this model takes none, or not that one --
/// so that "[Picture attached: chart.png]" is not followed by a model
/// describing a chart it was never sent.
std::string text_of(const ChatMessage& message, const Quirks& quirks) {
    if (message.images.empty() || !quirks.no_images) {
        return message.content;
    }
    return message.content + "\n\n[The picture" + (message.images.size() == 1 ? " was" : "s were")
         + " not sent: the provider would not take " + (message.images.size() == 1 ? "it" : "them")
         + ".]";
}

}  // namespace

std::string anthropic_body(std::string_view model, const ChatRequest& request,
                           const ModelFacts& facts, const Quirks& quirks) {
    // The Messages API takes the system prompt as a field of its own and the
    // conversation as turns that alternate. Crucible's history is close to
    // that already; this closes the gap -- system messages lifted out, a run
    // of same-role messages joined, and anything empty dropped, because an
    // empty text block is a 400.
    //
    // A message with pictures is a list of blocks, the pictures before the
    // text that asks about them; one without is a plain string, as it always
    // was.
    std::string system;
    json        turns = json::array();
    const auto blocks_of = [](json& content) {
        if (content.is_string()) {
            content = json::array({json{{"type", "text"}, {"text", content.get<std::string>()}}});
        }
    };
    for (const ChatMessage& message : request.messages) {
        const bool with_images = !message.images.empty() && message.role == "user" && !quirks.no_images;
        if (trimmed(message.content).empty() && !with_images) {
            continue;
        }
        if (message.role == "system") {
            system += (system.empty() ? "" : "\n\n") + message.content;
            continue;
        }
        const std::string role = message.role == "assistant" ? "assistant" : "user";
        json content = text_of(message, quirks);
        if (with_images) {
            content = json::array();
            for (const ChatImage& image : message.images) {
                content.push_back(json{{"type", "image"},
                                       {"source", json{{"type", "base64"},
                                                       {"media_type", image.mime},
                                                       {"data", image.data}}}});
            }
            if (!trimmed(message.content).empty()) {
                content.push_back(json{{"type", "text"}, {"text", message.content}});
            }
        }
        if (!turns.empty() && turns.back()["role"] == role) {
            json& previous = turns.back()["content"];
            if (previous.is_string() && content.is_string()) {
                previous = previous.get<std::string>() + "\n\n" + content.get<std::string>();
            } else {
                blocks_of(previous);
                blocks_of(content);
                for (json& block : content) {
                    previous.push_back(std::move(block));
                }
            }
        } else {
            turns.push_back(json{{"role", role}, {"content", std::move(content)}});
        }
    }

    json body;
    body["model"]  = model;
    body["stream"] = true;

    // "Until the model stops" has to be a number here, and a streamed reply
    // can afford a generous one. An explicit setting is respected as written.
    int limit = request.params.max_tokens > 0 ? request.params.max_tokens : kUnboundedReply;
    if (facts.max_output > 0) {
        limit = std::min(limit, facts.max_output);
    }
    body["max_tokens"] = limit;

    if (!system.empty()) {
        body["system"] = system;
    }
    body["messages"] = std::move(turns);

    // No temperature, top_p or top_k. The current Claude models reject them
    // outright, and the sliders on the Generation page are about sampling a
    // local model's logits -- a thing this request has no say in.

    if (!quirks.no_cache_control) {
        // A conversation resends its whole history each turn, and everything
        // but the last message is identical to the turn before. This asks the
        // API to notice.
        body["cache_control"] = json{{"type", "ephemeral"}};
    }
    if (facts.adaptive_thinking && !quirks.no_thinking) {
        // "summarized", because the default is to think invisibly: the reply
        // would begin with a pause of unexplained length. The summary is what
        // the transcript's thinking disclosure shows.
        body["thinking"] = json{{"type", "adaptive"}, {"display", "summarized"}};
    }
    if (facts.effort && !quirks.no_effort && is_effort(request.effort)) {
        body["output_config"] = json{{"effort", request.effort}};
    }
    if (facts.server_fallback && !quirks.no_fallbacks) {
        body["fallbacks"] = "default";
    }
    return dump(body);
}

std::string openai_body(std::string_view model, const ChatRequest& request,
                        const Quirks& quirks) {
    json messages = json::array();
    for (const ChatMessage& message : request.messages) {
        const bool with_images = !message.images.empty() && message.role == "user" && !quirks.no_images;
        if (trimmed(message.content).empty() && !with_images) {
            continue;
        }
        if (!with_images) {
            messages.push_back(json{{"role", message.role}, {"content", text_of(message, quirks)}});
            continue;
        }
        // The text first and the pictures after, each as a data URL: the one
        // form every server that takes pictures this way accepts.
        json content = json::array();
        if (!trimmed(message.content).empty()) {
            content.push_back(json{{"type", "text"}, {"text", message.content}});
        }
        for (const ChatImage& image : message.images) {
            content.push_back(json{{"type", "image_url"},
                                   {"image_url", json{{"url", "data:" + image.mime + ";base64," + image.data}}}});
        }
        messages.push_back(json{{"role", message.role}, {"content", std::move(content)}});
    }

    json body;
    body["model"]    = model;
    body["messages"] = std::move(messages);
    body["stream"]   = true;
    if (!quirks.no_stream_usage) {
        // Without this a streamed reply ends without saying what it cost, and
        // the token counts in the transcript would be guesses.
        body["stream_options"] = json{{"include_usage", true}};
    }
    if (!quirks.no_sampling) {
        // Rounded, because these are floats and JSON numbers are doubles: 0.7
        // would otherwise go out as 0.699999988079071, which is the same
        // number and an uglier line in somebody's request log.
        const auto tidy = [](float value) {
            return std::round(static_cast<double>(value) * 10000.0) / 10000.0;
        };
        body["temperature"] = tidy(request.params.temperature);
        body["top_p"]       = tidy(request.params.top_p);
        if (quirks.local_sampling) {
            // The rest of the Generation page, which llama.cpp is given and a
            // provider would refuse. Without the repetition penalty a model
            // on MLX's server wrote the same three lines until it was stopped.
            body["top_k"]              = request.params.top_k;
            body["min_p"]              = tidy(request.params.min_p);
            body["repetition_penalty"] = tidy(request.params.repeat_penalty);
            if (request.params.repeat_last_n > 0) {
                body["repetition_context_size"] = request.params.repeat_last_n;
            }
        }
    }
    const int limit = request.params.max_tokens > 0 ? request.params.max_tokens
                    : quirks.always_max_tokens       ? kUnboundedReply
                                                     : 0;
    if (limit > 0) {
        body[quirks.completion_tokens ? "max_completion_tokens" : "max_tokens"] = limit;
    }
    return dump(body);
}

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------

Event parse_anthropic(std::string_view data) {
    Event event;
    const json node = json::parse(data, nullptr, /*allow_exceptions=*/false);
    if (!node.is_object()) {
        return event;
    }
    const std::string type = string_at(node, "type");

    if (type == "content_block_delta") {
        const json&       delta = child(node, "delta");
        const std::string kind  = string_at(delta, "type");
        if (kind == "text_delta") {
            event.text = string_at(delta, "text");
        } else if (kind == "thinking_delta") {
            event.reasoning = string_at(delta, "thinking");
        }
        // signature_delta and input_json_delta are bookkeeping for features
        // Crucible does not use; they carry nothing to show.
    } else if (type == "content_block_start") {
        const json&       block = child(node, "content_block");
        const std::string kind  = string_at(block, "type");
        if (kind == "text") {
            event.text = string_at(block, "text");
        } else if (kind == "thinking") {
            event.reasoning = string_at(block, "thinking");
        } else if (kind == "fallback") {
            // The model that was asked declined, and another one is about to
            // answer in its place. Said in the transcript: which model wrote a
            // reply is not a detail to keep from the person reading it.
            const std::string from = string_at(child(block, "from"), "model");
            const std::string to   = string_at(child(block, "to"), "model");
            event.note = (from.empty() ? "the model" : from) + " declined this request, and "
                       + (to.empty() ? "another model" : to) + " answered instead";
        }
    } else if (type == "message_start") {
        const json& usage = child(child(node, "message"), "usage");
        if (usage.is_object()) {
            // What was read, wherever it was read from. The three are reported
            // apart because they are billed apart; the transcript wants the
            // size of the prompt.
            event.cache_read   = int_at(usage, "cache_read_input_tokens", 0);
            event.cache_write  = int_at(usage, "cache_creation_input_tokens", 0);
            event.input_tokens = int_at(usage, "input_tokens", 0) + event.cache_read + event.cache_write;
        }
    } else if (type == "message_delta") {
        const json& delta = child(node, "delta");
        event.stop          = string_at(delta, "stop_reason");
        event.output_tokens = int_at(child(node, "usage"), "output_tokens", -1);

        // Present only on a refusal, and not always then. Informational: what
        // is acted on is the stop reason.
        const json& details = child(delta, "stop_details");
        const std::string category    = string_at(details, "category");
        const std::string explanation = string_at(details, "explanation");
        event.detail = category;
        if (!explanation.empty()) {
            event.detail += (event.detail.empty() ? "" : ": ") + explanation;
        }
    } else if (type == "message_stop") {
        event.done = true;
    } else if (type == "error") {
        const json& error = child(node, "error");
        event.error = string_at(error, "message");
        if (event.error.empty()) {
            event.error = string_at(error, "type");
        }
        if (event.error.empty()) {
            event.error = "the stream reported an error";
        }
    }
    return event;
}

Event parse_openai(std::string_view data) {
    Event event;
    if (trimmed(data) == "[DONE]") {
        event.done = true;
        return event;
    }
    const json node = json::parse(data, nullptr, /*allow_exceptions=*/false);
    if (!node.is_object()) {
        return event;
    }
    if (const json& error = child(node, "error"); !error.is_null()) {
        event.error = error.is_string() ? error.get<std::string>() : string_at(error, "message");
        if (event.error.empty()) {
            event.error = "the stream reported an error";
        }
        return event;
    }

    const json& choices = child(node, "choices");
    if (choices.is_array() && !choices.empty()) {
        const json& choice = choices.front();
        const json& delta  = child(choice, "delta");
        event.text = string_at(delta, "content");
        // Where a reasoning model's working goes is the one thing the
        // chat-completions imitators did not agree on. DeepSeek and Moonshot
        // say reasoning_content; OpenRouter says reasoning.
        event.reasoning = string_at(delta, "reasoning_content");
        if (event.reasoning.empty()) {
            event.reasoning = string_at(delta, "reasoning");
        }
        event.stop = string_at(choice, "finish_reason");
    }
    if (const json& usage = child(node, "usage"); usage.is_object()) {
        event.input_tokens  = int_at(usage, "prompt_tokens", -1);
        event.output_tokens = int_at(usage, "completion_tokens", -1);
        // Part of prompt_tokens, not beside it, and billed for less.
        event.cache_read    = int_at(child(usage, "prompt_tokens_details"), "cached_tokens", -1);
    }
    return event;
}

// ---------------------------------------------------------------------------
// Learning what a provider takes
// ---------------------------------------------------------------------------

bool adapt(Quirks& quirks, std::string_view kind, std::string_view message) {
    const std::string said = format::to_lower(message);
    bool changed = false;
    const auto learn = [&changed](bool& quirk) {
        if (!quirk) {
            quirk   = true;
            changed = true;
        }
    };

    // A model that cannot take pictures says so in every wording there is --
    // "image input is not supported", "image_url is only supported by certain
    // models", "does not support images" -- and all of them say "image".
    if (contains(said, "image")) {
        learn(quirks.no_images);
    }

    if (kind == "anthropic") {
        if (contains(said, "fallback"))      { learn(quirks.no_fallbacks); }
        if (contains(said, "cache_control")) { learn(quirks.no_cache_control); }
        if (contains(said, "thinking"))      { learn(quirks.no_thinking); }
        if (contains(said, "output_config") || contains(said, "effort")) {
            learn(quirks.no_effort);
        }
        return changed;
    }

    if (contains(said, "max_tokens") && !contains(said, "max_completion_tokens is not")) {
        // "Unsupported parameter: 'max_tokens' ... use 'max_completion_tokens'
        // instead" is what the newer OpenAI models say.
        learn(quirks.completion_tokens);
    }
    if (contains(said, "temperature") || contains(said, "top_p")) {
        learn(quirks.no_sampling);
    }
    if (contains(said, "stream_options") || contains(said, "include_usage")) {
        learn(quirks.no_stream_usage);
    }
    return changed;
}

std::string error_message(std::string_view body) {
    const std::string_view text = trimmed(body);
    json node = json::parse(text, nullptr, /*allow_exceptions=*/false);
    // Google wraps its error in a one-element array.
    if (node.is_array() && !node.empty()) {
        node = node.front();
    }
    if (node.is_object()) {
        if (const json& error = child(node, "error"); !error.is_null()) {
            if (error.is_string()) {
                return error.get<std::string>();
            }
            if (std::string message = string_at(error, "message"); !message.empty()) {
                return message;
            }
        }
        // Cloudflare: {"errors": [{"message": "..."}]}
        if (const json& errors = child(node, "errors"); errors.is_array() && !errors.empty()) {
            if (std::string message = string_at(errors.front(), "message"); !message.empty()) {
                return message;
            }
        }
        for (const char* key : {"message", "detail", "msg"}) {
            if (std::string message = string_at(node, key); !message.empty()) {
                return message;
            }
        }
    }
    // Not JSON, or JSON nobody recognizes: an HTML error page from a proxy,
    // usually. The start of it is better than nothing and all of it is not.
    std::string start(text.substr(0, 300));
    std::replace(start.begin(), start.end(), '\n', ' ');
    return start;
}

ModelFacts facts_from_model(std::string_view body) {
    ModelFacts facts;
    const json node = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (!node.is_object()) {
        return facts;
    }
    facts.context_tokens = int_at(node, "max_input_tokens", 0);
    facts.max_output     = int_at(node, "max_tokens", 0);

    const json& capabilities = child(node, "capabilities");
    facts.adaptive_thinking =
        supported(child(child(child(capabilities, "thinking"), "types"), "adaptive"));
    facts.effort = supported(child(capabilities, "effort"));

    const json& fallbacks = child(node, "allowed_fallback_models");
    facts.server_fallback = fallbacks.is_array() && !fallbacks.empty();
    return facts;
}

std::vector<std::string> model_ids(std::string_view body) {
    std::vector<std::string> ids;
    const json node = json::parse(body, nullptr, /*allow_exceptions=*/false);

    // {"data": [...]} is what both formats say. The other two spellings are
    // what a few of the imitators say instead.
    const json* list = &node;
    for (const char* key : {"data", "models", "result"}) {
        if (const json& inner = child(node, key); inner.is_array()) {
            list = &inner;
            break;
        }
    }
    if (!list->is_array()) {
        return ids;
    }
    for (const json& entry : *list) {
        std::string id = entry.is_string() ? entry.get<std::string>() : string_at(entry, "id");
        if (id.empty()) {
            id = string_at(entry, "name");
        }
        // Gemini lists "models/gemini-2.5-pro" and then wants the bare name
        // back in a request.
        if (id.rfind("models/", 0) == 0) {
            id.erase(0, 7);
        }
        if (!id.empty()) {
            ids.push_back(std::move(id));
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

int estimate_tokens(const std::vector<ChatMessage>& messages) {
    // Three bytes a token is on the heavy side for English and about right
    // for code; a few more per message for the role and the separators.
    // A picture is about as many tokens as its pixels over 750, which for one
    // the window has shrunk to fit is at most about sixteen hundred.
    std::size_t bytes = 0;
    std::size_t images = 0;
    for (const ChatMessage& message : messages) {
        bytes += message.content.size();
        images += message.images.size();
    }
    return static_cast<int>(bytes / 3 + messages.size() * 8 + images * 1600 + 16);
}

}  // namespace detail

// ---------------------------------------------------------------------------
// One model at one provider
// ---------------------------------------------------------------------------

namespace {

/// The request in flight, so that it can be stopped from another thread.
/// The requests in flight, so Stop reaches the ones it means.
///
/// Several at once: a build's agents ask their providers side by side, and a
/// chat turn may be asking another while they do. Each request is entered
/// with the cancel switch of whoever made it, and interrupt() aborts the ones
/// whose switch is now on -- Stop in the chat does not cut an agent off
/// halfway through a file, and stopping a build leaves the chat alone. A
/// request entered with no switch is aborted by any interrupt, which is what
/// shutting down wants.
struct Flight {
    struct Entry {
        http::Stream*  stream = nullptr;
        CancelCallback cancel;
    };
    std::mutex         mutex;
    std::vector<Entry> entries;

    void enter(http::Stream* stream, CancelCallback cancel) {
        const std::lock_guard<std::mutex> lock(mutex);
        entries.push_back({stream, std::move(cancel)});
    }
    void leave(http::Stream* stream) {
        const std::lock_guard<std::mutex> lock(mutex);
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [stream](const Entry& e) { return e.stream == stream; }),
                      entries.end());
    }
    void interrupt() {
        const std::lock_guard<std::mutex> lock(mutex);
        for (const Entry& entry : entries) {
            if (!entry.cancel || entry.cancel()) {
                entry.stream->abort();
            }
        }
    }
};

std::vector<http::Header> headers_for(const Provider& provider, bool with_fallback_beta,
                                      const char* beta) {
    std::vector<http::Header> headers{{"content-type", "application/json"}};
    const std::string key = provider.resolved_key();
    if (provider.kind == "anthropic") {
        headers.push_back({"x-api-key", key});
        headers.push_back({"anthropic-version", kAnthropicVersion});
        if (with_fallback_beta) {
            headers.push_back({"anthropic-beta", beta});
        }
    } else if (!key.empty()) {
        // Optional on purpose. A llama.cpp or Ollama server on your own
        // network wants no key, and sending "Bearer " with nothing after it
        // is a malformed header some of them reject.
        headers.push_back({"authorization", "Bearer " + key});
    }
    return headers;
}

/// What to tell somebody about a refused request, given what the server said.
std::string describe_failure(const Provider& provider, int status, const std::string& said) {
    std::string out = provider.label();
    if (status == 401 || status == 403) {
        out += " refused the API key";
    } else if (status == 404) {
        out += " does not have that model";
    } else if (status == 429) {
        out += " is rate limiting this key";
    } else if (status >= 500) {
        out += " is having trouble (HTTP " + std::to_string(status) + ")";
    } else {
        out += " would not take the request (HTTP " + std::to_string(status) + ")";
    }
    if (!said.empty()) {
        out += ": " + said;
    }
    if (status == 401 || status == 403) {
        out += "  -- check it in Settings, Providers";
    }
    return out;
}

/// One model at one provider.
///
/// Asked from more than one thread at a time -- a build's agents share a
/// provider's model -- so what it learns as it goes, the facts and the
/// quirks, is kept under a lock and copied out for each request. A request
/// itself holds no lock: two agents asking Claude at once is the point.
///
/// Except for a server on this machine, which answers one request at a time
/// and is asked one at a time: two requests to MLX's server at once is two
/// generations fighting over one model's memory.
class Client final : public ChatModel {
public:
    /// `ledger` counts what is asked of it; see spend.hpp.
    Client(Provider provider, std::string model, Flight& flight, spend::Ledger& ledger)
        : provider_(std::move(provider)), model_(std::move(model)), flight_(flight), ledger_(&ledger) {}

    /// One whose facts and quirks are known before it is asked anything: a
    /// server on this machine, which publishes neither -- and costs nothing,
    /// so is not counted.
    Client(Provider provider, std::string model, Flight& flight, ModelFacts facts, Quirks quirks,
           bool serial)
        : provider_(std::move(provider)), model_(std::move(model)), flight_(flight),
          facts_(facts), facts_known_(true), quirks_(quirks), serial_(serial) {}

    ChatResult chat(const ChatRequest& request, const ChatSink& sink) override {
        ChatResult result;
        if (provider_.kind == "anthropic" && provider_.resolved_key().empty()) {
            result.error = provider_.label() + " has no API key -- add one in Settings, "
                           "Providers, or set ANTHROPIC_API_KEY";
            return result;
        }
        std::unique_lock<std::mutex> turn(turn_, std::defer_lock);
        if (serial_) {
            turn.lock();
        }
        learn_facts();

        // More than one attempt only when a provider named a field it will
        // not take and the request can be reshaped to do without it. Each
        // quirk is learned at most once, so this cannot go round forever; the
        // bound is belt and braces.
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (!run(request, sink, result)) {
                break;
            }
        }
        return result;
    }

    int prompt_tokens(const std::vector<ChatMessage>& messages) const override {
        return detail::estimate_tokens(messages);
    }

    int context_size() const override {
        const_cast<Client*>(this)->learn_facts();
        const std::lock_guard<std::mutex> lock(learned_);
        return facts_.context_tokens > 0 ? facts_.context_tokens : detail::kAssumedContext;
    }

    /// Claude sends its reasoning on a channel of its own and nowhere else.
    /// The chat-completions world is less tidy: the hosted reasoning models
    /// use a field, but an open model behind somebody's server -- Ollama,
    /// llama.cpp, half the hosts serving DeepSeek-R1 -- writes `<think>` into
    /// the answer exactly as it would on this machine.
    bool reasons_inline() const override { return provider_.kind != "anthropic"; }

    /// Until the provider refuses one. The refusal is a 400 that names the
    /// picture, which adapt() learns from, and the request goes again with
    /// the pictures left out -- the text still says what they were.
    bool sees_images() const override {
        const std::lock_guard<std::mutex> lock(learned_);
        return !quirks_.no_images;
    }

    /// Claude's that say so in the Models API. Nobody else is sent one.
    bool takes_effort() const override {
        const_cast<Client*>(this)->learn_facts();
        const std::lock_guard<std::mutex> lock(learned_);
        return provider_.kind == "anthropic" && facts_.effort && !quirks_.no_effort;
    }

private:
    bool official_anthropic() const {
        return provider_.kind == "anthropic"
            && provider_.endpoint() == "https://api.anthropic.com";
    }

    /// Ask Anthropic what this model takes. Once, and only of Anthropic:
    /// nobody else publishes it.
    void learn_facts() {
        {
            const std::lock_guard<std::mutex> lock(learned_);
            if (facts_known_ || provider_.kind != "anthropic") {
                return;
            }
        }
        // Asked without the lock held: two agents starting at once may both
        // ask, which costs a second listing, where holding the lock across a
        // request would make every other caller wait on the network.
        http::Request request;
        request.url             = provider_.endpoint() + "/v1/models/" + model_;
        request.timeout_seconds = 15;
        request.headers         = headers_for(provider_, official_anthropic(),
                                              kFallbackListingBeta);
        const http::Response response = http::send(request);
        const std::lock_guard<std::mutex> lock(learned_);
        if (response.ok()) {
            facts_ = detail::facts_from_model(response.body);
            // Only against Anthropic itself. A gateway may pass the listing
            // through and still not implement the feature it describes.
            facts_.server_fallback = facts_.server_fallback && official_anthropic();
            facts_known_ = true;
        } else if (response.status >= 400 && response.status < 500) {
            // A definite no -- a gateway with no Models API, a model it does
            // not list. Not worth asking again; the request goes out in its
            // plainest form, which every model accepts.
            facts_known_ = true;
        }
        // Anything else was the network, and the next prompt may have better
        // luck.
    }

    /// One attempt. True when it should be made again with what was learned.
    bool run(const ChatRequest& request, const ChatSink& sink, ChatResult& result) {
        const bool anthropic = provider_.kind == "anthropic";
        ModelFacts facts;
        Quirks     quirks;
        {
            const std::lock_guard<std::mutex> lock(learned_);
            facts  = facts_;
            quirks = quirks_;
        }
        const bool fallback = anthropic && facts.server_fallback && !quirks.no_fallbacks;

        http::Request outgoing;
        outgoing.method          = "POST";
        outgoing.timeout_seconds = 0;    // a long answer is not a hung request
        outgoing.connect_seconds = 20;
        outgoing.headers         = headers_for(provider_, fallback, kFallbackBeta);
        if (anthropic) {
            outgoing.url  = provider_.endpoint() + "/v1/messages";
            outgoing.body = detail::anthropic_body(model_, request, facts, quirks);
        } else {
            outgoing.url  = provider_.endpoint() + "/chat/completions";
            outgoing.body = detail::openai_body(model_, request, quirks);
        }

        http::Stream stream;
        std::string  open_error;
        if (!stream.open(outgoing, open_error)) {
            result.error = provider_.label() + " could not be asked: " + open_error;
            return false;
        }
        flight_.enter(&stream, sink.cancel);

        const Clock::time_point started = Clock::now();
        Clock::time_point       first_piece{};
        bool        any_piece    = false;
        bool        canceled     = false;
        int         input_tokens = -1;
        int         output_tokens = -1;
        int         cache_read   = 0;
        int         cache_write  = 0;
        std::size_t answer_bytes = 0;
        std::string stop;
        std::string stop_detail;
        std::string stream_error;
        std::string unframed;   // what arrived that was not an event: an error body

        const auto piece = [&]() {
            if (!any_piece) {
                any_piece   = true;
                first_piece = Clock::now();
            }
        };

        std::string line;
        while (stream.read_line(line)) {
            if (sink.cancel && sink.cancel()) {
                canceled = true;
                stream.abort();
                break;
            }
            if (line.rfind("data:", 0) != 0) {
                // "event:" lines repeat what the payload says, a ":" line is a
                // keep-alive, and a blank one ends an event. Anything else is
                // not a stream at all.
                if (!line.empty() && line.front() != ':' && line.rfind("event:", 0) != 0) {
                    unframed += line;
                    unframed += '\n';
                }
                continue;
            }
            const std::string_view data = trimmed(std::string_view(line).substr(5));
            const detail::Event event = anthropic ? detail::parse_anthropic(data)
                                                  : detail::parse_openai(data);
            if (!event.reasoning.empty()) {
                piece();
                if (sink.on_reasoning) {
                    sink.on_reasoning(event.reasoning);
                }
            }
            if (!event.text.empty()) {
                piece();
                answer_bytes += event.text.size();
                if (sink.on_text) {
                    sink.on_text(event.text);
                }
            }
            if (!event.note.empty())      { result.notes.push_back(event.note); }
            if (!event.stop.empty())      { stop = event.stop; }
            if (!event.detail.empty())    { stop_detail = event.detail; }
            if (!event.error.empty())     { stream_error = event.error; }
            if (event.input_tokens >= 0)  { input_tokens = event.input_tokens; }
            if (event.output_tokens >= 0) { output_tokens = event.output_tokens; }
            if (event.cache_read >= 0)    { cache_read = event.cache_read; }
            if (event.cache_write >= 0)   { cache_write = event.cache_write; }
        }
        flight_.leave(&stream);
        const http::Response response = stream.finish();
        const Clock::time_point ended = Clock::now();

        const auto ms = [](Clock::time_point from, Clock::time_point to) {
            return std::chrono::duration<double, std::milli>(to - from).count();
        };
        GenerationStats& stats = result.stats;
        stats.prompt_ms     += ms(started, any_piece ? first_piece : ended);
        stats.output_ms     += any_piece ? ms(first_piece, ended) : 0.0;
        stats.prompt_tokens  = input_tokens >= 0 ? input_tokens
                                                 : detail::estimate_tokens(request.messages);
        // A provider that would not say gets the same rough arithmetic the
        // prompt did, the other way up: four bytes a token for prose.
        stats.output_tokens += output_tokens >= 0 ? output_tokens
                                                  : static_cast<int>(answer_bytes / 4);
        count(response, input_tokens >= 0 ? input_tokens : stats.prompt_tokens, cache_read, cache_write,
              output_tokens >= 0 ? output_tokens : static_cast<int>(answer_bytes / 4), any_piece, facts);

        if (canceled || response.error == "stopped") {
            stats.canceled = true;
            return false;
        }
        if (response.status == 0 && !response.error.empty()) {
            result.error = provider_.label() + " could not be reached: " + response.error;
            return false;
        }
        if (response.status >= 400 || !stream_error.empty()) {
            const std::string said = !stream_error.empty() ? stream_error
                                                           : detail::error_message(unframed);
            // Only a 400, and only before anything was shown. A reshaped
            // request after half an answer would start the answer again
            // underneath the half already on screen.
            if (response.status == 400 && !any_piece) {
                const std::lock_guard<std::mutex> lock(learned_);
                if (detail::adapt(quirks_, provider_.kind, said)) {
                    return true;
                }
            }
            result.error = describe_failure(provider_, response.status, said);
            return false;
        }

        if (stop == "refusal") {
            // Not an error from the API's side -- the request succeeded and
            // the model, or a classifier in front of it, declined. What had
            // streamed before that is not an answer and is not kept as one.
            result.error = model_ + " declined this request";
            if (!stop_detail.empty()) {
                result.error += " (" + stop_detail + ")";
            }
            result.discard_partial = true;
            return false;
        }
        stats.hit_limit = stop == "max_tokens" || stop == "length";
        return false;
    }

    /// Put one request in the ledger: whatever the provider answered, it is
    /// what it bills for, a stopped answer's part included. A request that
    /// was refused before a word came back costs nothing, and only tells
    /// what is left of the rate limits.
    void count(const http::Response& response, int prompt, int cache_read, int cache_write, int output,
               bool answered, const ModelFacts& facts) {
        if (ledger_ == nullptr || response.status == 0) {
            return;
        }
        spend::Tokens used;
        if (response.status < 400 || answered) {
            const int fresh = std::max(0, prompt - cache_read - cache_write);
            used.input       = static_cast<std::uint64_t>(fresh);
            used.cache_read  = static_cast<std::uint64_t>(std::max(0, cache_read));
            used.cache_write = static_cast<std::uint64_t>(std::max(0, cache_write));
            used.output      = static_cast<std::uint64_t>(std::max(0, output));
            used.requests    = 1;
        }
        ledger_->record(provider_, model_, used, prompt,
                        facts.context_tokens > 0 ? facts.context_tokens : 0, response.headers);
    }

    Provider       provider_;
    std::string    model_;
    Flight&        flight_;
    spend::Ledger* ledger_ = nullptr;   ///< null for a server on this machine

    mutable std::mutex learned_;   ///< guards the three below
    ModelFacts         facts_;
    bool               facts_known_ = false;
    Quirks             quirks_;

    bool       serial_ = false;   ///< one request at a time; see the class comment
    std::mutex turn_;
};

/// The parts of a provider that decide whether what was learned still holds.
std::string fingerprint(const std::vector<Provider>& providers) {
    std::string out;
    for (const Provider& provider : providers) {
        out += provider.id + '\n' + provider.kind + '\n' + provider.endpoint() + '\n'
             + provider.api_key + "\n\n";
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Hub
// ---------------------------------------------------------------------------

/// Under one lock, because the build's agents and the chat ask for models
/// from their own threads.
///
/// A client is never destroyed while Crucible runs, only retired: adopt()
/// is a settings change, and an agent may be halfway through a request on
/// the client the change replaces. A retired client finishes its request
/// and is never handed out again; there are as many of them as there were
/// provider changes, which is a handful.
struct Hub::Impl {
    explicit Impl(std::filesystem::path spend_file) : ledger(std::move(spend_file)) {}

    std::mutex                                     mutex;
    std::vector<Provider>                          providers;
    std::string                                    fingerprint;
    std::map<std::string, std::unique_ptr<Client>> clients;
    std::vector<std::unique_ptr<Client>>           retired;
    Flight                                         flight;
    spend::Ledger                                  ledger;
};

Hub::Hub(std::filesystem::path spend_file) : impl_(std::make_unique<Impl>(std::move(spend_file))) {}
Hub::~Hub() = default;

std::vector<spend::Model> Hub::spending() const { return impl_->ledger.models(); }

void Hub::adopt(const Config& config) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    std::string now = fingerprint(config.providers);
    if (now == impl_->fingerprint) {
        // The list of models a provider offers can change without anything
        // here caring, and a settings slider moving must not throw away what
        // was learned about every model.
        impl_->providers = config.providers;
        return;
    }
    for (auto& [key, client] : impl_->clients) {
        impl_->retired.push_back(std::move(client));
    }
    impl_->clients.clear();
    impl_->providers   = config.providers;
    impl_->fingerprint = std::move(now);
}

ChatModel* Hub::model(const ModelParams& params, std::string& error) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    const Provider* provider = nullptr;
    for (const Provider& one : impl_->providers) {
        if (one.id == params.provider) {
            provider = &one;
        }
    }
    if (provider == nullptr) {
        error = "this expert is answered by a provider called \"" + params.provider
              + "\", and there is none -- add it in Settings, Providers";
        return nullptr;
    }
    const std::string key = provider->id + '\n' + params.model;
    auto found = impl_->clients.find(key);
    if (found == impl_->clients.end()) {
        found = impl_->clients
                    .emplace(key, std::make_unique<Client>(*provider, params.model,
                                                           impl_->flight, impl_->ledger))
                    .first;
    }
    return found->second.get();
}

ChatModel* Hub::local_server(const std::string& base_url, int context_tokens) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    const std::string key = "local\n" + base_url;
    auto found = impl_->clients.find(key);
    if (found == impl_->clients.end()) {
        Provider provider;
        provider.id       = "mlx";
        provider.name     = "MLX";
        provider.kind     = "openai";
        provider.base_url = base_url;
        ModelFacts facts;
        facts.context_tokens = context_tokens;
        Quirks quirks;
        quirks.no_images         = true;
        quirks.always_max_tokens = true;
        quirks.local_sampling    = true;
        found = impl_->clients
                    .emplace(key, std::make_unique<Client>(provider, "default_model", impl_->flight,
                                                           facts, quirks, true))
                    .first;
    }
    return found->second.get();
}

void Hub::interrupt() { impl_->flight.interrupt(); }

// ---------------------------------------------------------------------------
// Listing
// ---------------------------------------------------------------------------

std::vector<std::string> list_models(const Provider& provider, std::string& error) {
    http::Request request;
    request.timeout_seconds = 20;
    request.headers         = headers_for(provider, false, "");
    request.url = provider.kind == "anthropic"
                      // The most a page holds, which is more models than there are.
                      ? provider.endpoint() + "/v1/models?limit=1000"
                      : provider.endpoint() + "/models";

    const http::Response response = http::send(request);
    if (!response.ok()) {
        error = response.status == 0
                    ? provider.label() + " could not be reached: " + response.error
                    : describe_failure(provider, response.status,
                                       detail::error_message(response.body));
        return {};
    }
    std::vector<std::string> ids = detail::model_ids(response.body);
    if (ids.empty()) {
        error = provider.label() + " answered, but not with a list of models -- "
                "name the model by hand";
    }
    return ids;
}

}  // namespace crucible::remote
