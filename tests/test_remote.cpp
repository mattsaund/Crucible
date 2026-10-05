// SPDX-License-Identifier: MIT
//
// A model that is somewhere else, as far as it can be tested from here.
//
// Nothing in this file opens a connection. What it pins down is the two
// translations that can be wrong with no network at all: a conversation into
// a request body, and a line of a stream back into text. A mistake in the
// first is a 400 from somebody else's server; a mistake in the second is an
// answer that silently loses its reasoning, its token counts, or the fact
// that the model declined.
#include "test_helpers.hpp"

#include <nlohmann/json.hpp>

#include "crucible/llm/remote_model.hpp"

namespace {

using crucible::ChatMessage;
using crucible::ChatRequest;
using crucible::ModelParams;
using crucible::remote::ModelFacts;
using crucible::remote::Quirks;
namespace wire = crucible::remote::detail;
using json = nlohmann::json;

std::vector<ChatMessage> conversation() {
    return {
        {"system", "You are the physics expert."},
        {"user", "Why is the sky blue?"},
        {"assistant", "Rayleigh scattering."},
        {"user", "And at sunset?"},
    };
}

json anthropic(const std::vector<ChatMessage>& messages, const ModelParams& params,
               const ModelFacts& facts = {}, const Quirks& quirks = {},
               std::string_view effort = {}) {
    const ChatRequest request{messages, params, effort};
    return json::parse(wire::anthropic_body("claude-opus-5-5", request, facts, quirks));
}

json openai(const std::vector<ChatMessage>& messages, const ModelParams& params,
            const Quirks& quirks = {}) {
    const ChatRequest request{messages, params, {}};
    return json::parse(wire::openai_body("deepseek-chat", request, quirks));
}

}  // namespace

// --- the Messages shape ----------------------------------------------------

TEST(a_claude_request_lifts_the_system_prompt_out_of_the_turns) {
    const json body = anthropic(conversation(), ModelParams{});
    CHECK_EQ(body["model"].get<std::string>(), std::string("claude-opus-5-5"));
    CHECK_EQ(body["system"].get<std::string>(), std::string("You are the physics expert."));
    CHECK_EQ(body["messages"].size(), std::size_t{3});
    CHECK_EQ(body["messages"][0]["role"].get<std::string>(), std::string("user"));
    CHECK_EQ(body["messages"][2]["content"].get<std::string>(), std::string("And at sunset?"));
    CHECK(body["stream"].get<bool>());
}

TEST(a_claude_request_never_carries_sampling_settings) {
    // The current models answer 400 to any of these, and the sliders they
    // come from are about a local model's logits.
    ModelParams params;
    params.temperature = 0.2f;
    params.top_p       = 0.5f;
    params.top_k       = 10;
    const json body = anthropic(conversation(), params);
    CHECK(!body.contains("temperature"));
    CHECK(!body.contains("top_p"));
    CHECK(!body.contains("top_k"));
}

TEST(an_unbounded_claude_reply_is_given_a_number_and_the_model_caps_it) {
    ModelParams unbounded;
    unbounded.max_tokens = -1;
    CHECK_EQ(anthropic(conversation(), unbounded)["max_tokens"].get<int>(),
             wire::kUnboundedReply);

    ModelFacts small;
    small.max_output = 8192;
    CHECK_EQ(anthropic(conversation(), unbounded, small)["max_tokens"].get<int>(), 8192);

    ModelParams explicit_limit;
    explicit_limit.max_tokens = 500;
    CHECK_EQ(anthropic(conversation(), explicit_limit, small)["max_tokens"].get<int>(), 500);
}

TEST(a_claude_request_only_asks_for_what_the_model_said_it_takes) {
    // With nothing known, the plainest request there is.
    const json plain = anthropic(conversation(), ModelParams{}, {}, {}, "high");
    CHECK(!plain.contains("thinking"));
    CHECK(!plain.contains("output_config"));
    CHECK(!plain.contains("fallbacks"));

    ModelFacts facts;
    facts.adaptive_thinking = true;
    facts.effort            = true;
    facts.server_fallback   = true;
    const json full = anthropic(conversation(), ModelParams{}, facts, {}, "high");
    CHECK_EQ(full["thinking"]["type"].get<std::string>(), std::string("adaptive"));
    CHECK_EQ(full["thinking"]["display"].get<std::string>(), std::string("summarized"));
    CHECK(!full["thinking"].contains("budget_tokens"));
    CHECK_EQ(full["output_config"]["effort"].get<std::string>(), std::string("high"));
    CHECK_EQ(full["fallbacks"].get<std::string>(), std::string("default"));
    CHECK_EQ(full["cache_control"]["type"].get<std::string>(), std::string("ephemeral"));

    // "none" is Crucible's word for not reasoning at all, and not one the
    // API has.
    CHECK(!anthropic(conversation(), ModelParams{}, facts, {}, "none").contains("output_config"));
}

TEST(a_quirk_removes_its_field_from_a_claude_request) {
    ModelFacts facts;
    facts.adaptive_thinking = true;
    facts.effort            = true;
    facts.server_fallback   = true;
    Quirks quirks;
    quirks.no_thinking      = true;
    quirks.no_effort        = true;
    quirks.no_fallbacks     = true;
    quirks.no_cache_control = true;
    const json body = anthropic(conversation(), ModelParams{}, facts, quirks, "low");
    CHECK(!body.contains("thinking"));
    CHECK(!body.contains("output_config"));
    CHECK(!body.contains("fallbacks"));
    CHECK(!body.contains("cache_control"));
}

TEST(claude_turns_alternate_even_when_the_history_does_not) {
    // A retried turn or a tool result leaves two user messages in a row, and
    // an empty assistant message is what a stopped reply looks like. Either
    // is a 400 if it is sent as it stands.
    const std::vector<ChatMessage> messages = {
        {"user", "first"},
        {"user", "second"},
        {"assistant", ""},
        {"user", "third"},
    };
    const json body = anthropic(messages, ModelParams{});
    CHECK_EQ(body["messages"].size(), std::size_t{1});
    CHECK_EQ(body["messages"][0]["content"].get<std::string>(),
             std::string("first\n\nsecond\n\nthird"));
    CHECK(!body.contains("system"));
}

TEST(a_reply_that_is_not_utf8_cannot_break_the_next_request) {
    // A character cut in half at a token boundary, sitting in the history.
    const std::vector<ChatMessage> messages = {{"user", std::string("caf\xC3")}};
    const std::string body =
        wire::anthropic_body("claude-opus-5-5", ChatRequest{messages, ModelParams{}, {}}, {}, {});
    CHECK(json::parse(body, nullptr, false).is_object());
}

// --- the chat-completions shape --------------------------------------------

TEST(a_picture_goes_to_claude_as_a_block_before_the_text_that_asks_about_it) {
    std::vector<ChatMessage> messages = conversation();
    messages.back().images = {{"image/png", "iVBORw0K"}};
    const json body = anthropic(messages, ModelParams{});
    const json& content = body["messages"][2]["content"];
    CHECK(content.is_array());
    CHECK_EQ(content.size(), std::size_t{2});
    CHECK_EQ(content[0]["type"].get<std::string>(), std::string("image"));
    CHECK_EQ(content[0]["source"]["type"].get<std::string>(), std::string("base64"));
    CHECK_EQ(content[0]["source"]["media_type"].get<std::string>(), std::string("image/png"));
    CHECK_EQ(content[0]["source"]["data"].get<std::string>(), std::string("iVBORw0K"));
    CHECK_EQ(content[1]["text"].get<std::string>(), std::string("And at sunset?"));
    // The turns without pictures are plain strings, as they always were.
    CHECK(body["messages"][0]["content"].is_string());
}

TEST(two_user_turns_in_a_row_with_a_picture_join_as_blocks) {
    const std::vector<ChatMessage> messages{
        {"user", "First."},
        {"user", "Second, with a picture.", {{"image/jpeg", "/9j/"}}},
    };
    const json body = anthropic(messages, ModelParams{});
    CHECK_EQ(body["messages"].size(), std::size_t{1});
    const json& content = body["messages"][0]["content"];
    CHECK_EQ(content.size(), std::size_t{3});
    CHECK_EQ(content[0]["text"].get<std::string>(), std::string("First."));
    CHECK_EQ(content[1]["type"].get<std::string>(), std::string("image"));
    CHECK_EQ(content[2]["text"].get<std::string>(), std::string("Second, with a picture."));
}

TEST(a_picture_goes_to_chat_completions_as_a_data_url) {
    std::vector<ChatMessage> messages = conversation();
    messages.back().images = {{"image/webp", "UklGR"}};
    const json body = openai(messages, ModelParams{});
    const json& content = body["messages"].back()["content"];
    CHECK(content.is_array());
    CHECK_EQ(content[0]["type"].get<std::string>(), std::string("text"));
    CHECK_EQ(content[0]["text"].get<std::string>(), std::string("And at sunset?"));
    CHECK_EQ(content[1]["type"].get<std::string>(), std::string("image_url"));
    CHECK_EQ(content[1]["image_url"]["url"].get<std::string>(), std::string("data:image/webp;base64,UklGR"));
}

TEST(a_provider_that_refuses_pictures_is_sent_the_text_and_told_why) {
    Quirks quirks;
    CHECK(wire::adapt(quirks, "openai", "image input is not supported - hint: provide the mmproj"));
    CHECK(quirks.no_images);
    std::vector<ChatMessage> messages = conversation();
    messages.back().images = {{"image/png", "iVBORw0K"}};
    const json body = openai(messages, ModelParams{}, quirks);
    const json& last = body["messages"].back()["content"];
    CHECK(last.is_string());
    CHECK_EQ(last.get<std::string>(),
             std::string("And at sunset?\n\n[The picture was not sent: the provider would not take it.]"));

    // And the same for Claude behind a gateway that will not take them.
    Quirks gateway;
    CHECK(wire::adapt(gateway, "anthropic", "messages.0.content.0.image: Extra inputs are not permitted"));
    const json claude = anthropic(messages, ModelParams{}, {}, gateway);
    CHECK(claude["messages"][2]["content"].is_string());
}

TEST(a_picture_counts_toward_the_token_estimate) {
    std::vector<ChatMessage> messages = conversation();
    const int without = wire::estimate_tokens(messages);
    messages.back().images = {{"image/png", "x"}};
    CHECK(wire::estimate_tokens(messages) >= without + 1500);
}

TEST(a_chat_completions_request_keeps_the_system_message_in_line) {
    ModelParams params;
    params.temperature = 0.25f;
    params.max_tokens  = 300;
    const json body = openai(conversation(), params);
    CHECK_EQ(body["model"].get<std::string>(), std::string("deepseek-chat"));
    CHECK_EQ(body["messages"].size(), std::size_t{4});
    CHECK_EQ(body["messages"][0]["role"].get<std::string>(), std::string("system"));
    CHECK(body["stream"].get<bool>());
    CHECK(body["stream_options"]["include_usage"].get<bool>());
    CHECK_EQ(body["max_tokens"].get<int>(), 300);
    CHECK(body["temperature"].get<double>() > 0.24 && body["temperature"].get<double>() < 0.26);
}

TEST(an_unbounded_chat_completions_reply_names_no_limit) {
    ModelParams params;
    params.max_tokens = -1;
    const json body = openai(conversation(), params);
    CHECK(!body.contains("max_tokens"));
    CHECK(!body.contains("max_completion_tokens"));
}

TEST(a_quirk_reshapes_a_chat_completions_request) {
    ModelParams params;
    params.max_tokens = 300;
    Quirks quirks;
    quirks.completion_tokens = true;
    quirks.no_sampling       = true;
    quirks.no_stream_usage   = true;
    const json body = openai(conversation(), params, quirks);
    CHECK(!body.contains("max_tokens"));
    CHECK_EQ(body["max_completion_tokens"].get<int>(), 300);
    CHECK(!body.contains("temperature"));
    CHECK(!body.contains("top_p"));
    CHECK(!body.contains("stream_options"));
}

// --- reading a Messages stream ---------------------------------------------

TEST(a_claude_stream_yields_text_reasoning_and_usage) {
    CHECK_EQ(wire::parse_anthropic(
                 R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"Hello"}})")
                 .text,
             std::string("Hello"));
    CHECK_EQ(wire::parse_anthropic(
                 R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"Hmm"}})")
                 .reasoning,
             std::string("Hmm"));

    const auto start = wire::parse_anthropic(
        R"({"type":"message_start","message":{"usage":{"input_tokens":12,"cache_read_input_tokens":300,"cache_creation_input_tokens":8}}})");
    CHECK_EQ(start.input_tokens, 320);

    const auto end = wire::parse_anthropic(
        R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":41}})");
    CHECK_EQ(end.stop, std::string("end_turn"));
    CHECK_EQ(end.output_tokens, 41);

    CHECK(wire::parse_anthropic(R"({"type":"message_stop"})").done);
}

TEST(a_claude_stream_says_when_the_model_declined_and_why) {
    const auto event = wire::parse_anthropic(
        R"({"type":"message_delta","delta":{"stop_reason":"refusal","stop_details":{"type":"refusal","category":"cyber","explanation":"not something I can help with"}},"usage":{"output_tokens":3}})");
    CHECK_EQ(event.stop, std::string("refusal"));
    CHECK_EQ(event.detail, std::string("cyber: not something I can help with"));

    // The details are optional, and null is one of the ways they are absent.
    const auto bare = wire::parse_anthropic(
        R"({"type":"message_delta","delta":{"stop_reason":"refusal","stop_details":null}})");
    CHECK_EQ(bare.stop, std::string("refusal"));
    CHECK(bare.detail.empty());
}

TEST(a_claude_stream_says_when_another_model_answered) {
    const auto event = wire::parse_anthropic(
        R"({"type":"content_block_start","index":1,"content_block":{"type":"fallback","from":{"model":"claude-fable-5-1"},"to":{"model":"claude-opus-5-5"}}})");
    CHECK(event.note.find("claude-fable-5-1") != std::string::npos);
    CHECK(event.note.find("claude-opus-5-5") != std::string::npos);
    CHECK(event.text.empty());
}

TEST(a_claude_stream_error_is_an_error) {
    const auto event = wire::parse_anthropic(
        R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})");
    CHECK_EQ(event.error, std::string("Overloaded"));
}

TEST(a_stream_line_that_is_not_json_is_nothing_rather_than_a_crash) {
    CHECK(wire::parse_anthropic("not json").text.empty());
    CHECK(wire::parse_anthropic("[1,2,3]").text.empty());
    CHECK(wire::parse_anthropic(R"({"type":"content_block_delta","delta":"oops"})").text.empty());
    CHECK(wire::parse_openai("{").text.empty());
    CHECK(wire::parse_openai(R"({"choices":"oops"})").text.empty());
}

// --- reading a chat-completions stream -------------------------------------

TEST(a_chat_completions_stream_yields_text_and_ends_on_done) {
    const auto piece = wire::parse_openai(
        R"({"choices":[{"index":0,"delta":{"content":"Hi"},"finish_reason":null}]})");
    CHECK_EQ(piece.text, std::string("Hi"));
    CHECK(piece.stop.empty());

    const auto last = wire::parse_openai(
        R"({"choices":[{"index":0,"delta":{},"finish_reason":"length"}]})");
    CHECK_EQ(last.stop, std::string("length"));

    CHECK(wire::parse_openai(" [DONE] ").done);
}

TEST(a_chat_completions_stream_finds_reasoning_under_either_name) {
    CHECK_EQ(wire::parse_openai(
                 R"({"choices":[{"delta":{"reasoning_content":"thinking","content":null}}]})")
                 .reasoning,
             std::string("thinking"));
    CHECK_EQ(wire::parse_openai(R"({"choices":[{"delta":{"reasoning":"also"}}]})").reasoning,
             std::string("also"));
}

TEST(a_chat_completions_stream_reports_usage_from_the_chunk_with_no_choices) {
    const auto event = wire::parse_openai(
        R"({"choices":[],"usage":{"prompt_tokens":25,"completion_tokens":7,"total_tokens":32}})");
    CHECK_EQ(event.input_tokens, 25);
    CHECK_EQ(event.output_tokens, 7);
}

// --- learning what a provider takes ----------------------------------------

TEST(a_refused_field_is_learned_once) {
    Quirks quirks;
    CHECK(wire::adapt(quirks, "openai",
                        "Unsupported parameter: 'max_tokens' is not supported with this model. "
                        "Use 'max_completion_tokens' instead."));
    CHECK(quirks.completion_tokens);
    // The same complaint again changes nothing, which is what stops a retry
    // loop.
    CHECK(!wire::adapt(quirks, "openai", "Unsupported parameter: 'max_tokens'"));

    CHECK(wire::adapt(quirks, "openai",
                        "Unsupported value: 'temperature' does not support 0.7 with this model."));
    CHECK(quirks.no_sampling);
    CHECK(wire::adapt(quirks, "openai", "Unrecognized request argument: stream_options"));
    CHECK(quirks.no_stream_usage);
}

TEST(a_complaint_about_something_else_teaches_nothing) {
    Quirks quirks;
    CHECK(!wire::adapt(quirks, "openai", "The model `gpt-9` does not exist"));
    CHECK(!wire::adapt(quirks, "anthropic", "messages: at least one message is required"));
}

TEST(a_gateway_that_rejects_a_claude_field_is_remembered) {
    Quirks quirks;
    CHECK(wire::adapt(quirks, "anthropic", "fallbacks: Extra inputs are not permitted"));
    CHECK(quirks.no_fallbacks);
    CHECK(wire::adapt(quirks, "anthropic", "thinking.type: Input should be 'enabled'"));
    CHECK(quirks.no_thinking);
    CHECK(wire::adapt(quirks, "anthropic", "output_config: Extra inputs are not permitted"));
    CHECK(quirks.no_effort);
    CHECK(wire::adapt(quirks, "anthropic", "cache_control: Extra inputs are not permitted"));
    CHECK(quirks.no_cache_control);
}

// --- what a provider says about itself -------------------------------------

TEST(an_error_sentence_is_found_wherever_the_provider_put_it) {
    CHECK_EQ(wire::error_message(
                 R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})"),
             std::string("invalid x-api-key"));
    CHECK_EQ(wire::error_message(R"({"error":{"message":"Incorrect API key","code":"invalid_api_key"}})"),
             std::string("Incorrect API key"));
    CHECK_EQ(wire::error_message(R"([{"error":{"code":400,"message":"API key not valid"}}])"),
             std::string("API key not valid"));
    CHECK_EQ(wire::error_message(R"({"success":false,"errors":[{"code":10000,"message":"Authentication error"}]})"),
             std::string("Authentication error"));
    CHECK_EQ(wire::error_message(R"({"error":"model not found"})"), std::string("model not found"));
    CHECK_EQ(wire::error_message("<html>\nBad gateway</html>"),
             std::string("<html> Bad gateway</html>"));
}

TEST(a_models_entry_says_what_the_model_takes) {
    const ModelFacts facts = wire::facts_from_model(R"({
        "id": "claude-opus-5-5",
        "max_input_tokens": 1000000,
        "max_tokens": 128000,
        "capabilities": {
            "effort": {"supported": true},
            "thinking": {"supported": true, "types": {"adaptive": {"supported": true},
                                                      "enabled": {"supported": false}}}
        },
        "allowed_fallback_models": ["claude-opus-5"]
    })");
    CHECK_EQ(facts.context_tokens, 1000000);
    CHECK_EQ(facts.max_output, 128000);
    CHECK(facts.adaptive_thinking);
    CHECK(facts.effort);
    CHECK(facts.server_fallback);

    // An older model, or a gateway's idea of the same document.
    const ModelFacts bare = wire::facts_from_model(R"({"id":"claude-haiku-4-5","max_tokens":64000})");
    CHECK(!bare.adaptive_thinking);
    CHECK(!bare.effort);
    CHECK(!bare.server_fallback);
    CHECK_EQ(bare.context_tokens, 0);
}

TEST(a_model_listing_is_read_in_every_shape_it_comes_in) {
    const auto standard = wire::model_ids(
        R"({"data":[{"id":"gpt-5","object":"model"},{"id":"gpt-4.1"},{"id":"gpt-5"}]})");
    CHECK_EQ(standard.size(), std::size_t{2});
    CHECK_EQ(standard[0], std::string("gpt-4.1"));

    // Gemini prefixes; the request wants the bare name.
    const auto gemini = wire::model_ids(R"({"data":[{"id":"models/gemini-2.5-pro"}]})");
    CHECK_EQ(gemini.size(), std::size_t{1});
    CHECK_EQ(gemini[0], std::string("gemini-2.5-pro"));

    const auto ollama = wire::model_ids(R"({"models":[{"name":"qwen3:8b"}]})");
    CHECK_EQ(ollama.size(), std::size_t{1});
    CHECK_EQ(ollama[0], std::string("qwen3:8b"));

    CHECK(wire::model_ids("<html>").empty());
    CHECK(wire::model_ids(R"({"data":"nope"})").empty());
}

TEST(a_token_estimate_errs_high) {
    // Dropping history that would have fit costs a little context. Sending
    // more than fits costs the whole request.
    const std::vector<ChatMessage> messages = {{"user", std::string(3000, 'a')}};
    CHECK(wire::estimate_tokens(messages) >= 1000);
    CHECK(wire::estimate_tokens({}) > 0);
}

// --- the hub ---------------------------------------------------------------

TEST(a_seat_pointing_at_a_provider_that_is_not_there_says_so) {
    crucible::remote::Hub hub;
    crucible::Config config;
    hub.adopt(config);

    ModelParams params;
    params.provider = "nowhere";
    params.model    = "some-model";
    std::string error;
    CHECK(hub.model(params, error) == nullptr);
    CHECK(error.find("nowhere") != std::string::npos);
}

TEST(a_hub_hands_back_the_same_model_until_the_providers_change) {
    crucible::remote::Hub hub;
    crucible::Config config;
    crucible::Provider provider;
    provider.id       = "local";
    provider.kind     = "openai";
    provider.base_url = "http://127.0.0.1:9/v1";
    config.providers.push_back(provider);
    hub.adopt(config);

    ModelParams params;
    params.provider = "local";
    params.model    = "tiny";
    std::string error;
    crucible::ChatModel* first = hub.model(params, error);
    CHECK(first != nullptr);
    // An open model behind somebody's server writes <think> as it would here.
    CHECK(first->reasons_inline());
    CHECK_EQ(first->context_size(), wire::kAssumedContext);

    // A change that is not about the provider keeps what was learned.
    config.routing.min_confidence = 0.9F;
    hub.adopt(config);
    CHECK(hub.model(params, error) == first);

    // Stopping when nothing is running is not an error.
    hub.interrupt();
}
