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
///
/// The same header makes a model's own entry in the Models API list what it
/// may fall back to, which is how a model with none (Claude Haiku 5.5) is told
/// apart. The earlier 2026-06-01 header takes only a named list of models, and
/// the docs promise the listing under this one.
constexpr const char* kFallbackBeta = "server-side-fallback-2026-07-01";

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

/// Gemini, reached through its chat-completions door. Its own API beside the
/// door is the one that says how long a context a model takes.
bool gemini(const Provider& provider) {
    return provider.kind != "anthropic"
        && contains(format::to_lower(provider.endpoint()), "generativelanguage.googleapis.com");
}

/// Where a chat-completions request goes, as far as thinking is concerned:
/// most take `reasoning_effort`, and three are told another way.
enum class Door {
    Plain,       ///< reasoning_effort, "none" for no thinking where a model takes it
    DeepSeek,    ///< thinking: {type: enabled|disabled}, and reasoning_effort
    OpenRouter,  ///< reasoning: {effort} or {enabled: false}
    Local,       ///< a server on this machine or network, told through the template
};

Door door_of(std::string_view endpoint) {
    const std::string where = format::to_lower(endpoint);
    if (contains(where, "deepseek.com")) {
        return Door::DeepSeek;
    }
    if (contains(where, "openrouter.ai")) {
        return Door::OpenRouter;
    }
    Provider probe;
    probe.base_url = std::string(endpoint);
    return !where.empty() && probe.on_your_network() ? Door::Local : Door::Plain;
}

/// The words of the effort scale a provider's refusal lists as the ones it
/// takes, "Supported values are: 'low', 'medium', and 'high'" or "valid
/// values are high, low, medium, none", in the scale's order; and whether
/// "none" was among them. Empty when it lists none of them.
std::vector<std::string> listed_efforts(std::string_view said, bool& none) {
    none = false;
    std::size_t from = std::string_view::npos;
    for (const std::string_view lead : {std::string_view("supported values"), std::string_view("valid values"),
                                        std::string_view("must be one of"), std::string_view("one of:"),
                                        std::string_view("expected one of"), std::string_view("allowed values")}) {
        if (const std::size_t at = said.find(lead); at != std::string_view::npos) {
            from = std::min(from, at + lead.size());
        }
    }
    if (from == std::string_view::npos) {
        return {};
    }
    const std::string_view rest = said.substr(from);
    // A word is one when it stands alone: "xhigh" holds "high".
    const auto stands = [&rest](std::string_view word) {
        for (std::size_t at = rest.find(word); at != std::string_view::npos; at = rest.find(word, at + 1)) {
            const bool left  = at == 0 || std::isalnum(static_cast<unsigned char>(rest[at - 1])) == 0;
            const std::size_t end = at + word.size();
            const bool right = end >= rest.size() || std::isalnum(static_cast<unsigned char>(rest[end])) == 0;
            if (left && right) {
                return true;
            }
        }
        return false;
    };
    std::vector<std::string> out;
    for (const std::string& level : reasoning::scale()) {
        if (stands(level)) {
            out.push_back(level);
        }
    }
    none = stands("none");
    return out;
}

/// The list in a model listing, under whichever key the provider put it --
/// or the whole document, which is what Together sends. Null when there is
/// none.
const json* listing_in(const json& node) {
    if (node.is_array()) {
        return &node;
    }
    // {"data": [...]} is what both formats say. The other two spellings are
    // what a few of the imitators say instead.
    for (const char* key : {"data", "models", "result"}) {
        if (const json& inner = child(node, key); inner.is_array()) {
            return &inner;
        }
    }
    return nullptr;
}

/// A model's id as a request names it. Gemini lists "models/gemini-2.5-pro"
/// and then wants the bare name back.
std::string bare_id(std::string id) {
    if (id.rfind("models/", 0) == 0) {
        id.erase(0, 7);
    }
    return id;
}

/// A listed model's id, as a request names it.
std::string listed_id(const json& entry) {
    std::string id = entry.is_string() ? entry.get<std::string>() : string_at(entry, "id");
    if (id.empty()) {
        id = string_at(entry, "name");
    }
    return bare_id(std::move(id));
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
    // How hard it thinks, and whether: the level it takes nearest the one
    // asked for, and thinking as the box says where it can be switched.
    const reasoning::Profile profile = profile_of("anthropic", "", model, facts, quirks);
    std::string effort = reasoning::nearest(profile.levels, request.effort);
    const bool  off    = request.thinking == "off" && profile.can_switch() && !quirks.no_thinking;
    const bool  on     = request.thinking == "on" && !quirks.no_thinking;
    // A model described as taking a budget rather than an effort.
    const bool budget  = facts.thinking_budget && !facts.adaptive_thinking;
    if (off) {
        // "between_tools" where "disabled" is refused: Claude Sonnet 5.5
        // answers without thinking first that way. Neither is taken above
        // high, so a higher effort comes down to it.
        const bool between = facts.described && !facts.thinking_off && facts.between_tools;
        body["thinking"]   = json{{"type", between ? "between_tools" : "disabled"}};
        if (reasoning::rank(effort) > reasoning::rank("high")) {
            effort = reasoning::nearest(profile.levels, "high");
        }
    } else if (facts.adaptive_thinking && !quirks.no_thinking) {
        // "summarized", because the default is to think invisibly: the reply
        // would begin with a pause of unexplained length. The summary is what
        // the transcript's thinking disclosure shows.
        body["thinking"] = json{{"type", "adaptive"}, {"display", "summarized"}};
    } else if (const int tokens = thinking_budget(effort.empty() ? "medium" : effort, limit); on && budget && tokens > 0) {
        // Claude 4.5's Sonnet and Haiku and before: thinking is switched on
        // with a budget, which the level asked for sizes.
        body["thinking"] = json{{"type", "enabled"}, {"budget_tokens", tokens}};
    }
    // The effort itself, to a model that takes one. Not to one thinking on a
    // budget, where the budget is what the level became.
    if (facts.effort && !quirks.no_effort && !effort.empty()) {
        body["output_config"] = json{{"effort", effort}};
    }
    if (facts.server_fallback && !quirks.no_fallbacks) {
        body["fallbacks"] = "default";
    }
    return dump(body);
}

std::string openai_body(std::string_view model, const ChatRequest& request,
                        const ModelFacts& facts, const Quirks& quirks, std::string_view endpoint) {
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
    // How hard it thinks, and whether. Only to a model known to take it, by
    // its name or by what the provider has refused since: a field a server
    // does not know is a 400 at most of them. Gemini's door turns the effort
    // into the model's own thinking level.
    const reasoning::Profile profile = profile_of("openai", endpoint, model, facts, quirks);
    const std::string        effort  = quirks.no_effort ? std::string() : reasoning::nearest(profile.levels, request.effort);
    const bool               off     = request.thinking == "off" && profile.can_switch();
    const bool               on      = request.thinking == "on" && profile.can_switch();
    switch (door_of(endpoint)) {
        case Door::DeepSeek:
            // A switch of its own, and an effort beside it while it thinks.
            if ((off || on) && !quirks.no_think_switch) {
                body["thinking"] = json{{"type", off ? "disabled" : "enabled"}};
            }
            if (!off && !effort.empty()) {
                body["reasoning_effort"] = effort;
            }
            break;
        case Door::OpenRouter:
            // One object for both, which OpenRouter turns into whatever the
            // model behind it takes.
            if (off && !quirks.no_think_switch) {
                body["reasoning"] = json{{"enabled", false}};
            } else if (!effort.empty()) {
                body["reasoning"] = json{{"effort", effort}};
            } else if (on && !quirks.no_think_switch) {
                body["reasoning"] = json{{"enabled", true}};
            }
            break;
        case Door::Local:
            // llama-server and LM Studio hand these to the model's own
            // template, which is where a Qwen is switched; a server that does
            // not know the field says so once, and is not sent it again.
            if ((off || on) && !quirks.no_think_switch) {
                body["chat_template_kwargs"] = json{{"enable_thinking", on}};
            }
            if (!off && !effort.empty()) {
                body["reasoning_effort"] = effort;
            }
            break;
        case Door::Plain:
            if (off && profile.off_is_none && !quirks.no_none && !quirks.no_effort) {
                body["reasoning_effort"] = "none";
            } else if (!effort.empty()) {
                body["reasoning_effort"] = effort;
            } else if (on && !profile.thinks_unasked && profile.takes_effort() && !quirks.no_effort) {
                // A model that does not think unless asked (gpt-5.1), told
                // to at the level it would have used.
                body["reasoning_effort"] =
                    profile.standard.empty() ? reasoning::nearest(profile.levels, "medium") : profile.standard;
            }
            break;
    }
    return dump(body);
}

reasoning::Profile profile_of(std::string_view kind, std::string_view endpoint, std::string_view model,
                              const ModelFacts& facts, const Quirks& quirks) {
    reasoning::Profile profile;
    if (kind == "anthropic") {
        // Only what the Models API said it takes: a gateway in front of
        // Claude that does not describe its models is sent the plainest
        // request there is, which every one of them accepts.
        profile = facts.reasoning;
        if (profile.levels.empty() && facts.effort) {
            // It takes an effort without saying which: the three every
            // Claude that takes one does.
            profile.levels = {"low", "medium", "high"};
        }
        if (profile.thinking == reasoning::Thinking::None && (facts.adaptive_thinking || facts.thinking_budget)) {
            profile.thinking = facts.thinking_off || facts.between_tools ? reasoning::Thinking::Optional
                                                                         : reasoning::Thinking::Always;
            profile.thinks_unasked = facts.adaptive_thinking;
        }
        if (profile.source.empty() && (facts.effort || facts.adaptive_thinking || facts.thinking_budget)) {
            profile.source = "provider";
        }
    } else {
        profile = reasoning::from_name(endpoint, model);
        // A door known to take an effort for any model it serves, as
        // Gemini's does: the three every one of them takes.
        if (profile.levels.empty() && facts.effort) {
            profile.levels = {"low", "medium", "high"};
            if (profile.thinking == reasoning::Thinking::None) {
                profile.thinking = reasoning::Thinking::Always;
            }
        }
        // Whatever the provider said it takes when it refused a level is
        // what it takes, whatever the name suggested.
        if (!quirks.efforts.empty()) {
            profile.levels = quirks.efforts;
            if (std::find(profile.levels.begin(), profile.levels.end(), profile.standard) == profile.levels.end()) {
                profile.standard.clear();
            }
        }
        if (quirks.no_none && profile.off_is_none) {
            profile.off_is_none = false;
            profile.thinking    = reasoning::Thinking::Always;
        }
    }
    if (quirks.no_effort) {
        profile.levels.clear();
        profile.standard.clear();
    }
    if ((kind == "anthropic" && quirks.no_thinking) || (kind != "anthropic" && quirks.no_think_switch && !profile.off_is_none)) {
        if (profile.thinking == reasoning::Thinking::Optional) {
            profile.thinking = reasoning::Thinking::Always;
        }
    }
    return profile;
}

int thinking_budget(std::string_view level, int limit) {
    // How Claude's budgets have been mapped to efforts everywhere they have
    // been compared (Inspect, LiteLLM): a few thousand at the bottom, thirty
    // two thousand at the top.
    int tokens = 10000;
    switch (reasoning::rank(level)) {
        case 0: tokens = 2048;  break;   // minimal
        case 1: tokens = 4096;  break;   // low
        case 2: tokens = 10000; break;   // medium
        case 3: tokens = 16000; break;   // high
        case 4: tokens = 24000; break;   // xhigh
        case 5: tokens = 32000; break;   // max
        default: break;
    }
    // The budget is part of the reply's cap, and has to leave room for the
    // answer after it. 1024 is the least Anthropic takes.
    if (limit > 0) {
        tokens = std::min(tokens, limit - 1024);
    }
    return tokens >= 1024 ? tokens : 0;
}

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------

namespace {

/// What a Messages usage says was read, wherever it was read from. The three
/// are reported apart because they are billed apart; the transcript wants the
/// size of the prompt.
void read_prompt_usage(const json& usage, Event& event) {
    event.cache_read   = int_at(usage, "cache_read_input_tokens", 0);
    event.cache_write  = int_at(usage, "cache_creation_input_tokens", 0);
    event.input_tokens = int_at(usage, "input_tokens", 0) + event.cache_read + event.cache_write;
}

/// Every model that ran, from a Messages usage that lists them: the one asked
/// and the one that answered after it declined, or the second alone when a
/// conversation that fell back before is sent straight to it. Empty when no
/// fallback ran, whatever else the list holds.
std::vector<Attempt> attempts_in(const json& usage) {
    std::vector<Attempt> attempts;
    bool any_fallback = false;
    const json& iterations = child(usage, "iterations");
    if (!iterations.is_array()) {
        return attempts;
    }
    for (const json& one : iterations) {
        const std::string type = string_at(one, "type");
        if (type != "message" && type != "fallback_message") {
            continue;
        }
        Attempt attempt;
        attempt.model         = string_at(one, "model");
        attempt.fallback      = type == "fallback_message";
        attempt.input_tokens  = int_at(one, "input_tokens", 0);
        attempt.output_tokens = int_at(one, "output_tokens", 0);
        attempt.cache_read    = int_at(one, "cache_read_input_tokens", 0);
        attempt.cache_write   = int_at(one, "cache_creation_input_tokens", 0);
        any_fallback = any_fallback || attempt.fallback;
        if (!attempt.model.empty()) {
            attempts.push_back(std::move(attempt));
        }
    }
    if (!any_fallback) {
        attempts.clear();
    }
    return attempts;
}

/// Why a Messages reply stopped, from whatever holds `stop_reason`: the
/// stream's last delta, or a whole message.
void read_stop(const json& holder, Event& event) {
    event.stop = string_at(holder, "stop_reason");

    // Present only on a refusal, where its category and explanation may each
    // be null. Shown rather than read: what is acted on is the stop reason.
    const json& details = child(holder, "stop_details");
    const std::string category    = string_at(details, "category");
    const std::string explanation = string_at(details, "explanation");
    event.detail = category;
    if (!explanation.empty()) {
        event.detail += (event.detail.empty() ? "" : ": ") + explanation;
    }
    event.recommended = string_at(details, "recommended_model");
}

/// One block of a Messages reply, as it starts.
void read_block(const json& block, Event& event) {
    const std::string kind = string_at(block, "type");
    event.begun = true;
    if (kind == "text") {
        event.text = string_at(block, "text");
    } else if (kind == "thinking") {
        event.reasoning = string_at(block, "thinking");
    } else if (kind == "fallback") {
        // The model that was asked declined, and another carries on in its
        // place: from the start, or from where the first one stopped, whose
        // words then stand. Said in the transcript once it is known how that
        // went. Which model wrote a reply is not a detail to keep from the
        // person reading it.
        event.declined   = string_at(child(block, "from"), "model");
        event.takes_over = string_at(child(block, "to"), "model");
    }
}

/// The failure a chat-completions payload reports, or nothing.
std::string error_in(const json& node) {
    const json& error = child(node, "error");
    if (error.is_null()) {
        return {};
    }
    std::string said = error.is_string() ? error.get<std::string>() : string_at(error, "message");
    return said.empty() ? "the stream reported an error" : said;
}

/// What a chat-completions message, or a piece of one, says.
void read_said(const json& said, Event& event) {
    event.text = string_at(said, "content");
    // Where a reasoning model's working goes is the one thing the
    // chat-completions imitators did not agree on. DeepSeek and Moonshot say
    // reasoning_content; OpenRouter says reasoning.
    event.reasoning = string_at(said, "reasoning_content");
    if (event.reasoning.empty()) {
        event.reasoning = string_at(said, "reasoning");
    }
    // OpenAI's models decline in a field of their own, and leave the answer
    // empty.
    event.refusal = string_at(said, "refusal");
}

/// What a chat-completions usage says was read and written.
void read_openai_usage(const json& usage, Event& event) {
    event.input_tokens  = int_at(usage, "prompt_tokens", -1);
    event.output_tokens = int_at(usage, "completion_tokens", -1);
    // Part of prompt_tokens, not beside it, and billed for less.
    event.cache_read    = int_at(child(usage, "prompt_tokens_details"), "cached_tokens", -1);
}

}  // namespace

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
        read_block(child(node, "content_block"), event);
    } else if (type == "message_start") {
        if (const json& usage = child(child(node, "message"), "usage"); usage.is_object()) {
            read_prompt_usage(usage, event);
        }
    } else if (type == "message_delta") {
        read_stop(child(node, "delta"), event);
        const json& usage = child(node, "usage");
        event.output_tokens = int_at(usage, "output_tokens", -1);
        event.attempts      = attempts_in(usage);
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
    if (event.error = error_in(node); !event.error.empty()) {
        return event;
    }
    const json& choices = child(node, "choices");
    if (choices.is_array() && !choices.empty()) {
        const json& choice = choices.front();
        read_said(child(choice, "delta"), event);
        event.stop = string_at(choice, "finish_reason");
    }
    if (const json& usage = child(node, "usage"); usage.is_object()) {
        read_openai_usage(usage, event);
    }
    return event;
}

std::vector<Event> parse_reply(std::string_view kind, std::string_view body) {
    std::vector<Event> events;
    const json node = json::parse(trimmed(body), nullptr, /*allow_exceptions=*/false);
    if (!node.is_object()) {
        return events;
    }
    if (kind == "anthropic") {
        if (string_at(node, "type") != "message") {
            return events;
        }
        // The blocks in order, between what was read and how it ended, the
        // way the stream would have said them. A fallback block comes first
        // here even when the first model had started: a reply that is not
        // streamed leaves out what it wrote.
        const json& usage = child(node, "usage");
        Event start;
        read_prompt_usage(usage, start);
        events.push_back(std::move(start));
        if (const json& content = child(node, "content"); content.is_array()) {
            for (const json& block : content) {
                Event one;
                read_block(block, one);
                events.push_back(std::move(one));
            }
        }
        Event end;
        read_stop(node, end);
        end.output_tokens = int_at(usage, "output_tokens", -1);
        end.attempts      = attempts_in(usage);
        end.done          = true;
        events.push_back(std::move(end));
        return events;
    }

    Event event;
    event.error = error_in(node);
    const json& choices = child(node, "choices");
    const json& usage   = child(node, "usage");
    if (event.error.empty() && !(choices.is_array() && !choices.empty()) && !usage.is_object()) {
        return events;   // JSON, and not a reply
    }
    if (choices.is_array() && !choices.empty()) {
        const json& choice = choices.front();
        read_said(child(choice, "message"), event);
        event.stop = string_at(choice, "finish_reason");
    }
    if (usage.is_object()) {
        read_openai_usage(usage, event);
    }
    event.done = true;
    events.push_back(std::move(event));
    return events;
}

bool withheld(std::string_view stop) {
    // Anthropic's word, and OpenAI's -- which is everybody else's who copied
    // its finish reasons: DeepSeek's, OpenRouter's.
    return stop == "refusal" || stop == "content_filter";
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
    // The field a local server is told to switch thinking with, refused.
    if (contains(said, "chat_template_kwargs") || contains(said, "enable_thinking")) {
        learn(quirks.no_think_switch);
        return changed;
    }
    // A level it does not take, refused with the ones it does: kept, and
    // the nearest of them asked for instead. "none" missing from them is
    // thinking that cannot be switched off there.
    if (contains(said, "effort") || contains(said, "reasoning")) {
        bool                           none   = false;
        const std::vector<std::string> listed = listed_efforts(said, none);
        if (!listed.empty() && listed != quirks.efforts) {
            quirks.efforts = listed;
            changed        = true;
            if (!none) {
                learn(quirks.no_none);
            }
            return changed;
        }
    }
    // Anything else said of the effort, and of thinking, which a model that
    // does not think complains about rather than naming the field: neither is
    // sent again.
    if (contains(said, "reasoning_effort") || contains(said, "reasoning effort")
        || contains(said, "thinking")) {
        learn(quirks.no_effort);
        learn(quirks.no_think_switch);
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
    const json& types        = child(child(capabilities, "thinking"), "types");
    facts.adaptive_thinking  = supported(child(types, "adaptive"));
    facts.thinking_off       = supported(child(types, "disabled"));
    facts.between_tools      = supported(child(types, "between_tools"));
    facts.thinking_budget    = supported(child(types, "enabled"));
    facts.effort             = supported(child(capabilities, "effort"));
    facts.reasoning          = reasoning::from_anthropic(capabilities);
    facts.described          = capabilities.is_object();

    const json& fallbacks = child(node, "allowed_fallback_models");
    facts.server_fallback = fallbacks.is_array() && !fallbacks.empty();
    return facts;
}

std::vector<std::string> model_ids(std::string_view body) {
    std::vector<std::string> ids;
    const json node = json::parse(body, nullptr, /*allow_exceptions=*/false);
    const json* list = listing_in(node);
    if (list == nullptr) {
        return ids;
    }
    for (const json& entry : *list) {
        if (std::string id = listed_id(entry); !id.empty()) {
            ids.push_back(std::move(id));
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

std::string facts_url(const Provider& provider, std::string_view model) {
    const std::string endpoint = provider.endpoint();
    if (provider.kind == "anthropic") {
        return endpoint + "/v1/models/" + std::string(model);
    }
    if (gemini(provider)) {
        // ".../v1beta/openai" is the door, and ".../v1beta/models/<id>" the
        // model's own entry, which says its inputTokenLimit.
        std::string base = endpoint;
        constexpr std::string_view door = "/openai";
        if (base.size() >= door.size() && base.compare(base.size() - door.size(), door.size(), door) == 0) {
            base.erase(base.size() - door.size());
        }
        return base + "/models/" + bare_id(std::string(model));
    }
    // One listing for every model rather than a lookup of one: every
    // provider has the listing, and not all of them have the lookup.
    return endpoint + "/models";
}

ModelFacts facts_from_listing(std::string_view body, std::string_view model) {
    ModelFacts facts;
    const json node = json::parse(body, nullptr, /*allow_exceptions=*/false);
    const json* entry = nullptr;
    if (const json* list = listing_in(node)) {
        const std::string wanted = bare_id(std::string(model));
        const auto names = [&wanted](const json& one) {
            if (listed_id(one) == wanted) {
                return true;
            }
            // Mistral lists "mistral-large-latest" as an alias of the model
            // it currently means.
            const json& aliases = child(one, "aliases");
            return aliases.is_array()
                && std::any_of(aliases.begin(), aliases.end(), [&wanted](const json& alias) {
                       return alias.is_string() && alias.get<std::string>() == wanted;
                   });
        };
        const auto found = std::find_if(list->begin(), list->end(), names);
        if (found != list->end()) {
            entry = &*found;
        }
    } else if (node.is_object()) {
        entry = &node;   // one model's own entry, asked for by name
    }
    if (entry == nullptr) {
        return facts;
    }
    // Each says it in its own words: OpenRouter, Together and Fireworks
    // context_length, Groq context_window, Mistral max_context_length,
    // Gemini inputTokenLimit, Anthropic max_input_tokens.
    for (const char* key : {"context_length", "context_window", "max_context_length",
                            "inputTokenLimit", "max_input_tokens"}) {
        if (const int tokens = int_at(*entry, key, 0); tokens > 0) {
            facts.context_tokens = tokens;
            return facts;
        }
    }
    // DeepInfra keeps it a level down.
    facts.context_tokens = std::max(0, int_at(child(*entry, "metadata"), "context_length", 0));
    return facts;
}

int known_context(std::string_view model) {
    // The last word, after the provider has been asked. A table of names is
    // out of date the week it is written, so this one holds only families
    // that are the same wherever they are served, and a model it does not
    // know is assumed 128k as before. An open model behind somebody's server
    // runs with whatever context the server was started with, which no
    // table knows.
    //
    // Each figure is what a prompt may fill: the window, less the reply's
    // share where the provider keeps one back. The first that fits wins, so
    // a model comes before the family it would otherwise fall into. Checked
    // against each provider's own pages on 2026-10-26, named above each group.
    struct Known {
        std::string_view prefix;
        int              tokens;
    };
    static constexpr Known kKnown[] = {
        // developers.openai.com/api/docs/models/<id>. The 1,050,000 windows
        // keep 128,000 for the reply ("Maximum input tokens: 922,000", where
        // the page says), the 400,000 ones the same.
        {"gpt-6", 922000},
        {"gpt-5.6-cyber", 272000}, {"gpt-5.6", 922000}, {"gpt-5.5", 922000},
        {"gpt-5.4-mini", 272000}, {"gpt-5.4-nano", 272000}, {"gpt-5.4", 922000},
        {"gpt-5-chat", 128000}, {"gpt-5", 272000}, {"gpt-4.1", 1047576},
        {"gpt-4o", 128000}, {"gpt-4-turbo", 128000},
        {"o1-mini", 128000}, {"o1", 200000}, {"o3", 200000}, {"o4-mini", 200000},
        // Anthropic's own API says; this is for a gateway in front of it
        // with no Models API. platform.claude.com/docs/en/build-with-claude/
        // context-windows: 1M from the 4.6 models on, 200k before them, in
        // either spelling a gateway gives the version.
        {"claude-opus-4-5", 200000}, {"claude-opus-4.5", 200000},
        {"claude-sonnet-4-5", 200000}, {"claude-sonnet-4.5", 200000},
        {"claude-haiku-4-5", 200000}, {"claude-haiku-4.5", 200000},
        {"claude-3", 200000}, {"claude-", 1000000},
        // ai.google.dev/gemini-api/docs/models: every model that writes text.
        {"gemini-", 1048576},
        // api-docs.deepseek.com/quick_start/pricing: "1M". DeepSeek's own API
        // retired the two older names on 2026-07-24 (news/news260424).
        {"deepseek-v4", 1000000}, {"deepseek-flash", 1000000},
        {"deepseek-chat", 128000}, {"deepseek-reasoner", 128000},
        // docs.x.ai/developers/models. The names retired on 2026-05-15 are
        // answered by grok-4.3 since, and grok-code-fast by grok-build
        // (migration/may-15-retirement); grok-3-mini is on neither page and
        // keeps what it had.
        {"grok-4.20", 1000000}, {"grok-4.3", 1000000},
        {"grok-4-fast", 1000000}, {"grok-4-1-fast", 1000000}, {"grok-4-0709", 1000000},
        {"grok-build", 256000}, {"grok-code-fast", 256000},
        {"grok-4", 500000}, {"grok-3-mini", 131072}, {"grok-3", 1000000},
        // platform.kimi.ai/docs/pricing/chat: 1,048,576 for K3, 262,144 for
        // K2. The first K2 preview, discontinued, had less.
        {"kimi-k3", 1048576}, {"kimi-k2-0711", 131072}, {"kimi-k2", 262144},
    };
    // The name without whose it is: "openai/gpt-4o" is "gpt-4o".
    std::string name = format::to_lower(model);
    if (const std::size_t slash = name.rfind('/'); slash != std::string::npos) {
        name.erase(0, slash + 1);
    }
    for (const Known& known : kKnown) {
        if (name.rfind(known.prefix, 0) != 0) {
            continue;
        }
        // The whole of a part of the name: "o1" is not "o10".
        const std::size_t end = known.prefix.size();
        if (name.size() == end || known.prefix.back() == '-' || name[end] == '-' || name[end] == '.') {
            return known.tokens;
        }
    }
    return 0;
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
/// Several at once: a chat turn asks its provider while the API server asks
/// another. Each request is entered with the cancel switch of whoever made
/// it, and interrupt() aborts the ones whose switch is now on -- Stop in the
/// chat does not cut off what the server is answering. A request entered
/// with no switch is aborted by any interrupt, which is what shutting down
/// wants.
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

std::vector<http::Header> headers_for(const Provider& provider, bool with_fallback_beta) {
    std::vector<http::Header> headers{{"content-type", "application/json"}};
    const std::string key = provider.resolved_key();
    if (provider.kind == "anthropic") {
        headers.push_back({"x-api-key", key});
        headers.push_back({"anthropic-version", kAnthropicVersion});
        if (with_fallback_beta) {
            headers.push_back({"anthropic-beta", kFallbackBeta});
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
        out += " (check it in Settings, Providers)";
    }
    return out;
}

/// Names in a sentence: "a", "a and b", "a, b and c".
std::string listed(const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) {
            out += i + 1 == names.size() ? " and " : ", ";
        }
        out += names[i];
    }
    return out;
}

/// One model at one provider.
///
/// Asked from more than one thread at a time -- the chat and the API server
/// share a provider's model -- so what it learns as it goes, the facts and
/// the quirks, is kept under a lock and copied out for each request. A
/// request itself holds no lock: two callers asking Claude at once is the
/// point.
///
/// Except for a server on this machine, which answers one request at a time
/// and is asked one at a time: two requests to MLX's server at once is two
/// generations fighting over one model's memory.
class Client final : public ChatModel {
public:
    /// `ledger` counts what is asked of it; see spend.hpp.
    Client(Provider provider, std::string model, Flight& flight, spend::Ledger& ledger)
        : provider_(std::move(provider)), model_(std::move(model)), flight_(flight), ledger_(&ledger) {
        if (gemini(provider_)) {
            // Google asks that Gemini 3 be left at its own temperature and
            // says a lower one can make it loop; Gemini 4 ignores the field.
            // The Generation page's sliders are about a local model's logits
            // anyway. What its door does take is an effort, which it turns
            // into the model's thinking level: see reasoning::from_name.
            quirks_.no_sampling = true;
        }
    }

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
            result.error = provider_.label() + " has no API key: add one in Settings, "
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

    /// What the provider said, or what the model is known to have, or 128k.
    int context_size() const override {
        const_cast<Client*>(this)->learn_facts();
        const std::lock_guard<std::mutex> lock(learned_);
        const int known = context_known(facts_);
        return known > 0 ? known : detail::kAssumedContext;
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

    /// Claude's that say so in the Models API, and the others known by name
    /// to take one, until the provider refuses it.
    bool takes_effort() const override {
        return reasoning().takes_effort();
    }

    /// What the request is shaped by: see detail::profile_of.
    reasoning::Profile reasoning() const override {
        const_cast<Client*>(this)->learn_facts();
        const std::lock_guard<std::mutex> lock(learned_);
        return detail::profile_of(provider_.kind, provider_.endpoint(), model_, facts_, quirks_);
    }

private:
    bool official_anthropic() const {
        return provider_.kind == "anthropic"
            && provider_.endpoint() == "https://api.anthropic.com";
    }

    /// The context as the provider said it, or as the model is known by name
    /// to have; 0 when neither says.
    int context_known(const ModelFacts& facts) const {
        return facts.context_tokens > 0 ? facts.context_tokens : detail::known_context(model_);
    }

    /// Ask the provider what this model is, once. Anthropic's Models API says
    /// what it takes as well as how long a context it has; Gemini's own API
    /// and the listings that carry it say the context; the listings that do
    /// not cost one request a session.
    void learn_facts() {
        const bool anthropic = provider_.kind == "anthropic";
        {
            const std::lock_guard<std::mutex> lock(learned_);
            if (facts_known_) {
                return;
            }
            // A listing that did not answer is not asked again for a minute:
            // each round of a turn would otherwise wait out its timeout.
            if (asked_at_ != std::chrono::steady_clock::time_point{}
                && std::chrono::steady_clock::now() - asked_at_ < std::chrono::minutes(1)) {
                return;
            }
            asked_at_ = std::chrono::steady_clock::now();
        }
        // Asked without the lock held: two callers starting at once may both
        // ask, which costs a second listing, where holding the lock across a
        // request would make every other caller wait on the network.
        http::Request request;
        request.url             = detail::facts_url(provider_, model_);
        request.timeout_seconds = 15;
        if (anthropic) {
            request.headers = headers_for(provider_, official_anthropic());
        } else if (gemini(provider_)) {
            // Google's own API takes the key in a header of its own; the
            // bearer token the door takes is refused there.
            if (const std::string key = provider_.resolved_key(); !key.empty()) {
                request.headers.push_back({"x-goog-api-key", key});
            }
        } else {
            request.headers = headers_for(provider_, false);
        }
        const http::Response response = http::send(request);
        const std::lock_guard<std::mutex> lock(learned_);
        if (response.ok()) {
            if (anthropic) {
                facts_ = detail::facts_from_model(response.body);
                // Only against Anthropic itself. A gateway may pass the listing
                // through and still not implement the feature it describes.
                facts_.server_fallback = facts_.server_fallback && official_anthropic();
            } else {
                // Only the context: what else is known of these, a listing
                // does not say.
                facts_.context_tokens = detail::facts_from_listing(response.body, model_).context_tokens;
            }
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
        outgoing.headers         = headers_for(provider_, fallback);
        if (anthropic) {
            outgoing.url  = provider_.endpoint() + "/v1/messages";
            outgoing.body = detail::anthropic_body(model_, request, facts, quirks);
        } else {
            outgoing.url  = provider_.endpoint() + "/chat/completions";
            outgoing.body = detail::openai_body(model_, request, facts, quirks, provider_.endpoint());
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
        bool        framed       = false;   // whether anything came as a stream's event
        bool        canceled     = false;
        int         input_tokens = -1;
        int         output_tokens = -1;
        int         cache_read   = 0;
        int         cache_write  = 0;
        std::size_t answer_bytes = 0;
        std::string stop;
        std::string stop_detail;
        std::string refusal;       // the model's own words declining
        std::string recommended;   // a model to ask instead, when a refusal names one
        std::string stream_error;
        std::string unframed;      // what arrived that was not an event: an error body, or a whole reply
        std::vector<std::pair<std::string, std::string>> handovers;   // who declined, and who took over
        std::vector<detail::Attempt> attempts;

        const auto piece = [&]() {
            if (!any_piece) {
                any_piece   = true;
                first_piece = Clock::now();
            }
        };
        const auto take = [&](const detail::Event& event) {
            if (event.begun) {
                piece();   // the clock for the reply starts with its first block, shown or not
            }
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
            if (!event.declined.empty() || !event.takes_over.empty()) {
                handovers.emplace_back(event.declined.empty() ? model_ : event.declined, event.takes_over);
            }
            refusal += event.refusal;
            if (!event.stop.empty())        { stop = event.stop; }
            if (!event.detail.empty())      { stop_detail = event.detail; }
            if (!event.recommended.empty()) { recommended = event.recommended; }
            if (!event.attempts.empty())    { attempts = event.attempts; }
            if (!event.error.empty())       { stream_error = event.error; }
            if (event.input_tokens >= 0)    { input_tokens = event.input_tokens; }
            if (event.output_tokens >= 0)   { output_tokens = event.output_tokens; }
            if (event.cache_read >= 0)      { cache_read = event.cache_read; }
            if (event.cache_write >= 0)     { cache_write = event.cache_write; }
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
            framed = true;
            const std::string_view data = trimmed(std::string_view(line).substr(5));
            take(anthropic ? detail::parse_anthropic(data) : detail::parse_openai(data));
        }
        flight_.leave(&stream);
        const http::Response response = stream.finish();
        // Asked for a stream, a server that does not stream sends the reply
        // whole: one document where the events would have been.
        if (!framed && !canceled && response.status == 200) {
            for (const detail::Event& event : detail::parse_reply(provider_.kind, unframed)) {
                take(event);
            }
        }
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
              output_tokens >= 0 ? output_tokens : static_cast<int>(answer_bytes / 4), any_piece, facts,
              attempts);

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

        if (detail::withheld(stop) || !trimmed(refusal).empty()) {
            // Not an error from the API's side -- the request succeeded and
            // the model, or a classifier in front of it, declined. What had
            // streamed before that is not an answer and is not kept as one.
            // A model that took over and declined too is named with the one
            // it took over from.
            std::vector<std::string> declined;
            for (const auto& handover : handovers) {
                declined.push_back(handover.first);
            }
            declined.push_back(handovers.empty() || handovers.back().second.empty() ? model_
                                                                                  : handovers.back().second);
            result.error = listed(declined) + " declined this request";
            const std::string why = !stop_detail.empty()            ? stop_detail
                                  : !trimmed(refusal).empty()       ? std::string(trimmed(refusal))
                                  : stop == "content_filter"        ? std::string("content filter")
                                                                    : std::string();
            if (!why.empty()) {
                result.error += " (" + why + ")";
            }
            // Named when the provider could not hand the request on itself,
            // its fallback being busy: a seat can be pointed at it by hand.
            if (!recommended.empty()) {
                result.error += ". Try " + recommended;
            }
            result.discard_partial = true;
            return false;
        }
        for (const auto& [from, to] : handovers) {
            result.notes.push_back(from + " declined, " + (to.empty() ? "another model" : to) + " answered");
        }
        if (handovers.empty()) {
            // A conversation that fell back before may be sent straight to the
            // model that answered it then. Nothing declined this time, so only
            // the count of who ran says so.
            for (const detail::Attempt& attempt : attempts) {
                if (attempt.fallback) {
                    result.notes.push_back(attempt.model + " answered in place of " + model_);
                }
            }
        }
        stats.hit_limit = stop == "max_tokens" || stop == "length";
        return false;
    }

    /// Put one request in the ledger: whatever the provider answered, it is
    /// what it bills for, a stopped answer's part included. A request that
    /// was refused before a word came back costs nothing, and only tells
    /// what is left of the rate limits.
    ///
    /// When another model answered in place of this one, each model that ran
    /// is counted under its own name: each attempt is billed apart, at its
    /// own model's rates, and the totals the response gives are only the
    /// last one's.
    void count(const http::Response& response, int prompt, int cache_read, int cache_write, int output,
               bool answered, const ModelFacts& facts, const std::vector<detail::Attempt>& attempts) {
        if (ledger_ == nullptr || response.status == 0) {
            return;
        }
        if (!attempts.empty() && response.status < 400) {
            for (std::size_t i = 0; i < attempts.size(); ++i) {
                const detail::Attempt& attempt = attempts[i];
                // The first, unless the model asked never ran, is the model
                // asked, under the name its seat gives it. The rate limits
                // the headers report go with the first, whoever that was.
                const bool asked = i == 0 && !attempt.fallback;
                spend::Tokens used;
                used.input       = static_cast<std::uint64_t>(std::max(0, attempt.input_tokens));
                used.cache_read  = static_cast<std::uint64_t>(std::max(0, attempt.cache_read));
                used.cache_write = static_cast<std::uint64_t>(std::max(0, attempt.cache_write));
                used.output      = static_cast<std::uint64_t>(std::max(0, attempt.output_tokens));
                used.requests    = 1;
                const std::string& name = asked ? model_ : attempt.model;
                ledger_->record(provider_, name, used,
                                attempt.input_tokens + attempt.cache_read + attempt.cache_write,
                                asked ? context_known(facts) : detail::known_context(name),
                                i == 0 ? response.headers : std::vector<http::Header>{});
            }
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
        ledger_->record(provider_, model_, used, prompt, context_known(facts), response.headers);
    }

    Provider       provider_;
    std::string    model_;
    Flight&        flight_;
    spend::Ledger* ledger_ = nullptr;   ///< null for a server on this machine

    mutable std::mutex learned_;   ///< guards the four below
    ModelFacts         facts_;
    bool               facts_known_ = false;
    std::chrono::steady_clock::time_point asked_at_{};   ///< when the listing was last asked for
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

/// Under one lock, because the chat and the API server ask for models from
/// their own threads.
///
/// A client is never destroyed while Crucible runs, only retired: adopt()
/// is a settings change, and a turn may be halfway through a request on
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
              + "\", and there is none: add it in Settings, Providers";
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
    request.headers         = headers_for(provider, false);
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
        error = provider.label() + " answered, but not with a list of models: "
                "name the model by hand";
    }
    return ids;
}

reasoning::Profile provider_profile(const Provider& provider, std::string_view model) {
    if (provider.kind != "anthropic") {
        return detail::profile_of(provider.kind, provider.endpoint(), model, ModelFacts{}, Quirks{});
    }
    // What the Models API said of each Claude, kept for the session: the box
    // asks every time an expert is chosen, and the answer does not change.
    static std::mutex                        mutex;
    static std::map<std::string, ModelFacts> described;
    const std::string key = provider.endpoint() + '\n' + std::string(model);
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (const auto found = described.find(key); found != described.end()) {
            return detail::profile_of("anthropic", provider.endpoint(), model, found->second, Quirks{});
        }
    }
    ModelFacts facts;
    if (!provider.resolved_key().empty()) {
        http::Request request;
        request.url             = detail::facts_url(provider, model);
        request.timeout_seconds = 15;
        request.headers         = headers_for(provider, false);
        const http::Response response = http::send(request);
        if (response.ok()) {
            facts = detail::facts_from_model(response.body);
        } else if (response.status == 0) {
            // The network, this time: asked again the next time, and known
            // by name meanwhile.
            return detail::profile_of("anthropic", provider.endpoint(), model, facts, Quirks{});
        }
    }
    const std::lock_guard<std::mutex> lock(mutex);
    described[key] = facts;
    return detail::profile_of("anthropic", provider.endpoint(), model, facts, Quirks{});
}

}  // namespace crucible::remote
