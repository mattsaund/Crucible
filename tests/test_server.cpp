// SPDX-License-Identifier: MIT
//
// The API server, called the way another program calls it: over HTTP, on a
// port of its own, with the app's own client (curl underneath).
//
// The server is asked of a stand-in model -- one that says what it was told,
// with a line of thinking first, and one that calls the tools it is offered
// and embeds what it is given -- so every way a request can go is one a test
// can walk in a moment and with no weights: the models list, an answer whole
// and streamed, a tool call whole and in deltas, the calls and results of a
// conversation, a JSON response_format, pictures, embeddings, a key wrong and
// right, an open server, a body too large or too deep, a model that is not
// the one served, stop sequences, a port already taken, IPv4 and IPv6, and
// HTTPS with a certificate made here or brought, kept and checked. Where
// CRUCIBLE_TEST_MODEL names a small GGUF, requests go through the engine to
// that model as well, tools and JSON and embeddings with them.
#include "test_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include "crucible/engine/engine.hpp"
#include "crucible/llm/open_chat.hpp"
#include "crucible/serve/server.hpp"
#include "crucible/serve/tls.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/http.hpp"

namespace {

using json = nlohmann::json;

/// A model that needs no weights: it thinks one line, then says what it
/// was told, a word at a time, and stops at max_tokens words.
class StandIn final : public serve::Backend {
public:
    std::string model() const override { return "stand-in.gguf"; }

    serve::Answer answer(const serve::Ask& ask, const serve::Sink& sink) override {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            asked = ask;
        }
        sink.waiting("");
        const std::string told = ask.raw ? ask.prompt : ask.messages.empty() ? "" : ask.messages.back().content;
        if (!ask.raw) {
            sink.reasoning("Thinking it over.");
        }
        std::vector<std::string> words;
        std::istringstream       in("You said: " + told + " END");
        for (std::string word; in >> word;) {
            words.push_back(word);
        }
        serve::Answer out;
        out.prompt_tokens = static_cast<int>(told.size() / 4) + 1;
        const int most    = ask.max_tokens.value_or(-1);
        for (std::size_t i = 0; i < words.size(); ++i) {
            if (sink.cancel()) {
                break;
            }
            if (most > 0 && out.completion_tokens == most) {
                out.hit_limit = true;
                break;
            }
            sink.answer((i == 0 ? "" : " ") + words[i]);
            ++out.completion_tokens;
        }
        return out;
    }

    serve::Ask last() {
        const std::lock_guard<std::mutex> lock(mutex);
        return asked;
    }

private:
    std::mutex mutex;
    serve::Ask asked;
};

/// A server on a free port, answered by the stand-in, for as long as this
/// lives.
struct Running {
    StandIn       model;
    serve::Server server{model};
    int           port = 0;

    explicit Running(const std::string& key = {}, bool network = false) {
        serve::Settings settings;
        settings.enabled = true;
        settings.port    = 0;
        settings.network = network;
        settings.key     = key;
        server.apply(settings);
        port = server.status().port;
    }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port) + path;
    }
};

util::http::Response get(const std::string& url, const std::string& key = {}) {
    util::http::Request request;
    request.url = url;
    if (!key.empty()) {
        request.headers.push_back({"Authorization", "Bearer " + key});
    }
    return util::http::send(request);
}

util::http::Response post(const std::string& url, const std::string& body, const std::string& key = {}) {
    util::http::Request request;
    request.method = "POST";
    request.url    = url;
    request.body   = body;
    request.headers.push_back({"Content-Type", "application/json"});
    if (!key.empty()) {
        request.headers.push_back({"Authorization", "Bearer " + key});
    }
    request.timeout_seconds = 120;
    return util::http::send(request);
}

json parsed(const util::http::Response& response) {
    return json::parse(response.body, nullptr, false);
}

/// A streamed answer: each event's data, parsed, and whether it ended with
/// [DONE].
struct Events {
    std::vector<json> data;
    bool              done = false;
    int               status = 0;
};

Events stream(const std::string& url, const json& body, const std::string& key = {}) {
    util::http::Request request;
    request.method = "POST";
    request.url    = url;
    request.body   = body.dump();
    request.headers.push_back({"Content-Type", "application/json"});
    if (!key.empty()) {
        request.headers.push_back({"Authorization", "Bearer " + key});
    }
    request.timeout_seconds = 120;
    Events             out;
    util::http::Stream reading;
    std::string        error;
    if (!reading.open(request, error)) {
        return out;
    }
    for (std::string line; reading.read_line(line);) {
        if (line.rfind("data: ", 0) != 0) {
            continue;
        }
        const std::string data = line.substr(6);
        if (data == "[DONE]") {
            out.done = true;
            continue;
        }
        out.data.push_back(json::parse(data, nullptr, false));
    }
    out.status = reading.finish().status;
    return out;
}

/// What a stream said, put back together: the content, the reasoning, and
/// how it finished.
struct Told {
    std::string content;
    std::string reasoning;
    std::string finish;
    json        usage;
    std::string role;
};

Told told_by(const Events& events) {
    Told out;
    for (const json& event : events.data) {
        if (event.contains("usage") && event["usage"].is_object()) {
            out.usage = event["usage"];
        }
        if (!event.contains("choices") || event["choices"].empty()) {
            continue;
        }
        const json& choice = event["choices"][0];
        if (choice.contains("delta")) {
            const json& delta = choice["delta"];
            out.content   += delta.value("content", "");
            out.reasoning += delta.value("reasoning_content", "");
            if (delta.contains("role")) {
                out.role = delta["role"].get<std::string>();
            }
        } else {
            out.content += choice.value("text", "");
        }
        if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
            out.finish = choice["finish_reason"].get<std::string>();
        }
    }
    return out;
}

bool curl_here() {
    std::string why;
    if (util::http::available(why)) {
        return true;
    }
    std::printf("      skipped: %s\n", why.c_str());
    return false;
}

json chat(const std::string& said, json extra = json::object()) {
    json body{{"model", "stand-in.gguf"},
              {"messages", json::array({json{{"role", "system"}, {"content", "Be brief."}},
                                        json{{"role", "user"}, {"content", said}}})}};
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        body[it.key()] = it.value();
    }
    return body;
}

/// A tool a request offers, as OpenAI's API has one.
json weather_tool(const std::string& name = "get_weather") {
    return json{{"type", "function"},
                {"function", {{"name", name},
                              {"description", "The weather in a city"},
                              {"parameters", {{"type", "object"},
                                              {"properties", {{"city", {{"type", "string"}}}}},
                                              {"required", json::array({"city"})}}}}}};
}

/// A model that calls the first tool it is offered, its arguments a piece
/// at a time, after a word; and that embeds a text as its length and the
/// count of its spaces, scaled to unit length.
class Caller final : public serve::Backend {
public:
    std::string model() const override { return "caller.gguf"; }
    std::string embedder() const override { return "embedder.gguf"; }

    serve::Answer answer(const serve::Ask& ask, const serve::Sink& sink) override {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            asked = ask;
        }
        sink.waiting("");
        serve::Answer out;
        out.prompt_tokens = 7;
        if (ask.embed) {
            for (const std::string& text : ask.texts) {
                const float length = static_cast<float>(text.size());
                const auto  spaces = static_cast<float>(std::count(text.begin(), text.end(), ' '));
                const float norm   = std::sqrt(length * length + spaces * spaces + 1.0F);
                out.vectors.push_back({length / norm, spaces / norm, 1.0F / norm});
            }
            return out;
        }
        if (ask.tools.is_null() || ask.tool_choice == "none") {
            sink.answer("No tools.");
            out.completion_tokens = 2;
            return out;
        }
        const std::string name = ask.tools[0]["function"]["name"].get<std::string>();
        sink.answer("Checking.");
        if (ask.chat.is_array() && !ask.chat.empty() && ask.chat.back().value("content", "") == "Cut short") {
            out.completion_tokens = 2;   // the limit came before the call
            out.hit_limit         = true;
            return out;
        }
        sink.call(serve::CallPiece{0, "call_abc", name, ""});
        for (const char* piece : {"{\"city\":", " \"Pa", "ris\"}"}) {
            sink.call(serve::CallPiece{0, "", "", piece});
        }
        out.completion_tokens = 9;
        return out;
    }

    serve::Ask last() {
        const std::lock_guard<std::mutex> lock(mutex);
        return asked;
    }

private:
    std::mutex mutex;
    serve::Ask asked;
};

/// A server on a free port answered by the caller.
struct Calling {
    Caller        model;
    serve::Server server{model};
    int           port = 0;

    explicit Calling(bool network = false) {
        serve::Settings settings;
        settings.enabled = true;
        settings.port    = 0;
        settings.network = network;
        server.apply(settings);
        port = server.status().port;
    }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port) + path;
    }
};

/// What a stream's tool calls add up to, by index: OpenAI's shape, as a
/// client puts the deltas back together.
json calls_of(const Events& events) {
    json calls = json::array();
    for (const json& event : events.data) {
        if (!event.contains("choices") || event["choices"].empty()) {
            continue;
        }
        const json& delta = event["choices"][0].value("delta", json::object());
        for (const json& call : delta.value("tool_calls", json::array())) {
            const auto index = call.value("index", 0);
            while (calls.size() <= static_cast<std::size_t>(index)) {
                calls.push_back(json{{"id", ""}, {"name", ""}, {"arguments", ""}});
            }
            json& into = calls[static_cast<std::size_t>(index)];
            if (call.contains("id")) {
                into["id"] = into["id"].get<std::string>() + call["id"].get<std::string>();
            }
            const json function = call.value("function", json::object());
            if (function.contains("name")) {
                into["name"] = into["name"].get<std::string>() + function["name"].get<std::string>();
            }
            into["arguments"] = into["arguments"].get<std::string>() + function.value("arguments", "");
        }
    }
    return calls;
}

}  // namespace

TEST(a_tool_offered_reaches_the_model_and_its_call_comes_back_whole) {
    if (!curl_here()) {
        return;
    }
    Calling running;
    json body = chat("What is the weather in Paris?", {{"tools", json::array({weather_tool()})}});
    body["model"] = "caller.gguf";
    const util::http::Response answered = post(running.url("/v1/chat/completions"), body.dump());
    CHECK_EQ(answered.status, 200);
    const json reply   = parsed(answered);
    const json message = reply["choices"][0]["message"];
    CHECK(reply["choices"][0].value("finish_reason", "") == "tool_calls");
    CHECK(message.value("content", "") == "Checking.");
    CHECK(message["tool_calls"].size() == 1);
    CHECK(message["tool_calls"][0].value("id", "") == "call_abc");
    CHECK(message["tool_calls"][0].value("type", "") == "function");
    CHECK(message["tool_calls"][0]["function"].value("name", "") == "get_weather");
    CHECK(message["tool_calls"][0]["function"].value("arguments", "") == R"({"city": "Paris"})");

    // What the model was asked: the tools as sent, and the conversation in
    // OpenAI's shape beside the text of it.
    const serve::Ask asked = running.model.last();
    CHECK(asked.tools.is_array() && asked.tools.size() == 1);
    CHECK_EQ(asked.tool_choice, "auto");
    CHECK(!asked.parallel_tool_calls.has_value());
    CHECK(asked.chat.is_array() && asked.chat.size() == 2);
    CHECK(asked.chat[1].value("content", "") == "What is the weather in Paris?");
}

TEST(a_tool_call_streams_as_deltas_a_client_puts_back_together) {
    if (!curl_here()) {
        return;
    }
    Calling running;
    json body = chat("Weather?", {{"tools", json::array({weather_tool()})}, {"stream", true}});
    body.erase("model");
    const Events events = stream(running.url("/v1/chat/completions"), body);
    CHECK_EQ(events.status, 200);
    CHECK(events.done);
    const json calls = calls_of(events);
    CHECK(calls.size() == 1);
    if (calls.size() == 1) {
        // The id and the name once, whole, and the arguments as they came.
        CHECK(calls[0].value("id", "") == "call_abc");
        CHECK(calls[0].value("name", "") == "get_weather");
        CHECK(calls[0].value("arguments", "") == R"({"city": "Paris"})");
    }
    const Told told = told_by(events);
    CHECK_EQ(told.content, "Checking.");
    CHECK_EQ(told.finish, "tool_calls");
}

TEST(a_chat_template_that_will_not_write_a_conversation_says_why_in_its_own_words) {
    // As llama.cpp's Jinja reports a template that refuses: the template
    // quoted around the place it stopped, then the reason, which is all a
    // client can use.
    bool raised = false;
    const std::string refusal =
        "\n------------\nWhile executing CallExpression at line 1, column 191 in source:\n"
        "...'system' else 0)) -%}{{- raise_exception('Conversation roles must alternate user...\n"
        "                                           ^\n"
        "Error: Jinja Exception: Conversation roles must alternate user/assistant/user/assistant/...";
    CHECK_EQ(detail::template_said(refusal, &raised),
             "Conversation roles must alternate user/assistant/user/assistant/...");
    CHECK(raised);

    // One that fails rather than refuses, and errors that are not a template's.
    const std::string failure = "\n------------\nWhile executing FilterExpression at line 2, column 5 in source:\n"
                                "{{ x | nope }}\n    ^\nError: Unknown filter 'nope'";
    CHECK_EQ(detail::template_said(failure, &raised), "Unknown filter 'nope'");
    CHECK(!raised);
    CHECK(detail::template_said("JSON schema conversion failed").empty());
    CHECK(detail::template_said("std::exception").empty());

    // One that cannot be read says why, from llama.cpp's own reader: loading
    // a model passes the reason on as only "std::exception".
    const std::string unreadable = detail::why_unreadable("{% if %}<|im_start|>{{ messages }}<|im_end|>");
    CHECK(!unreadable.empty());
    CHECK(unreadable.find("std::exception") == std::string::npos);
    CHECK(unreadable.find('\n') == std::string::npos);
    CHECK(detail::why_unreadable("{% for message in messages %}{{ message['content'] }}{% endfor %}").empty());
}

TEST(a_required_call_comes_back_alone_without_the_words_said_before_it) {
    if (!curl_here()) {
        return;
    }
    Calling running;
    json body = chat("Weather?", {{"tools", json::array({weather_tool()})}, {"tool_choice", "required"}});
    body.erase("model");

    // Whole: the call, and content null, as OpenAI sends a required call.
    const json reply   = parsed(post(running.url("/v1/chat/completions"), body.dump()));
    const json message = reply["choices"][0]["message"];
    CHECK(message.contains("content") && message["content"].is_null());
    CHECK(message["tool_calls"].size() == 1);
    CHECK(reply["choices"][0].value("finish_reason", "") == "tool_calls");

    // Streamed: the call's deltas, and no words.
    body["stream"] = true;
    const Events events = stream(running.url("/v1/chat/completions"), body);
    CHECK(events.done);
    CHECK(calls_of(events).size() == 1);
    const Told told = told_by(events);
    CHECK_EQ(told.content, "");
    CHECK_EQ(told.finish, "tool_calls");

    // A call that never came (the limit met first): the words are all there
    // is, so they come back, whole and streamed.
    body = chat("Cut short", {{"tools", json::array({weather_tool()})}, {"tool_choice", "required"}});
    body.erase("model");
    const json cut = parsed(post(running.url("/v1/chat/completions"), body.dump()));
    CHECK(cut["choices"][0]["message"].value("content", "") == "Checking.");
    CHECK(!cut["choices"][0]["message"].contains("tool_calls"));
    CHECK(cut["choices"][0].value("finish_reason", "") == "length");
    body["stream"] = true;
    const Told cut_told = told_by(stream(running.url("/v1/chat/completions"), body));
    CHECK_EQ(cut_told.content, "Checking.");
    CHECK_EQ(cut_told.finish, "length");
}

TEST(a_named_tool_choice_offers_that_tool_alone_and_requires_it) {
    if (!curl_here()) {
        return;
    }
    Calling running;
    json body = chat("Hello", {{"tools", json::array({weather_tool("get_weather"), weather_tool("get_time")})},
                               {"tool_choice", {{"type", "function"}, {"function", {{"name", "get_time"}}}}},
                               {"parallel_tool_calls", false}});
    body.erase("model");
    const json reply = parsed(post(running.url("/v1/chat/completions"), body.dump()));
    CHECK(reply["choices"][0]["message"]["tool_calls"][0]["function"].value("name", "") == "get_time");
    const serve::Ask asked = running.model.last();
    CHECK(asked.tools.size() == 1);
    CHECK_EQ(asked.tool_choice, "required");
    CHECK(asked.parallel_tool_calls.has_value() && !*asked.parallel_tool_calls);

    // A function that is not offered, a choice of no known kind, and the old
    // names for tools are refused, naming the parameter.
    body["tool_choice"] = {{"type", "function"}, {"function", {{"name", "get_mood"}}}};
    const util::http::Response missing = post(running.url("/v1/chat/completions"), body.dump());
    CHECK_EQ(missing.status, 400);
    CHECK(parsed(missing)["error"].value("param", "") == "tool_choice");
    body["tool_choice"] = "sometimes";
    CHECK_EQ(post(running.url("/v1/chat/completions"), body.dump()).status, 400);
    json old = chat("Hello", {{"functions", json::array({weather_tool()["function"]})}});
    old.erase("model");
    const util::http::Response renamed = post(running.url("/v1/chat/completions"), old.dump());
    CHECK_EQ(renamed.status, 400);
    CHECK(parsed(renamed)["error"].value("param", "") == "functions");
    json bad = chat("Hello", {{"tools", json::array({json{{"type", "function"}, {"function", json::object()}}})}});
    bad.erase("model");
    CHECK_EQ(post(running.url("/v1/chat/completions"), bad.dump()).status, 400);

    // "none" is taken, and the model answers in words.
    body = chat("Hello", {{"tools", json::array({weather_tool()})}, {"tool_choice", "none"}});
    body.erase("model");
    const json words = parsed(post(running.url("/v1/chat/completions"), body.dump()));
    CHECK(words["choices"][0]["message"].value("content", "") == "No tools.");
    CHECK(words["choices"][0].value("finish_reason", "") == "stop");
}

TEST(a_conversation_with_calls_and_their_results_is_read_in_openai_shape) {
    if (!curl_here()) {
        return;
    }
    Calling running;
    const json messages = json::array(
        {json{{"role", "developer"}, {"content", "Use the tools."}},
         json{{"role", "user"}, {"content", "Weather in Paris?"}},
         json{{"role", "assistant"},
              {"content", nullptr},
              {"tool_calls", json::array({json{{"id", "call_1"},
                                               {"type", "function"},
                                               {"function", {{"name", "get_weather"},
                                                             {"arguments", R"({"city":"Paris"})"}}}}})}},
         json{{"role", "tool"}, {"tool_call_id", "call_1"}, {"content", R"({"temperature": 18})"}}});
    const json body{{"messages", messages}, {"tools", json::array({weather_tool()})}};
    CHECK_EQ(post(running.url("/v1/chat/completions"), body.dump()).status, 200);
    const serve::Ask asked = running.model.last();
    CHECK(asked.chat.size() == 4);
    if (asked.chat.size() == 4) {
        CHECK(asked.chat[0].value("role", "") == "developer");   // as it came, for the template
        CHECK(asked.chat[2]["tool_calls"][0]["function"].value("arguments", "") == R"({"city":"Paris"})");
        CHECK(asked.chat[2]["content"].is_null());
        CHECK(asked.chat[3].value("tool_call_id", "") == "call_1");
    }
    CHECK(asked.messages.size() == 4);
    if (asked.messages.size() == 4) {
        CHECK_EQ(asked.messages[0].role, "system");   // as a model asked the chat's way reads it
        CHECK_EQ(asked.messages[3].role, "tool");
    }

    // A result that names no call, and an assistant that says nothing and
    // calls nothing, are refused.
    json orphan = body;
    orphan["messages"][3].erase("tool_call_id");
    CHECK_EQ(post(running.url("/v1/chat/completions"), orphan.dump()).status, 400);
    json empty = body;
    empty["messages"][2].erase("tool_calls");
    CHECK_EQ(post(running.url("/v1/chat/completions"), empty.dump()).status, 400);
}

TEST(a_json_response_format_becomes_the_schema_the_answer_is_held_to) {
    if (!curl_here()) {
        return;
    }
    Calling running;
    json body = chat("A cat, as JSON", {{"response_format", {{"type", "json_object"}}}});
    body.erase("model");
    CHECK_EQ(post(running.url("/v1/chat/completions"), body.dump()).status, 200);
    const json object_only{{"type", "object"}};
    CHECK(json::parse(running.model.last().schema) == object_only);

    const json schema{{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}}}, {"required", {"name"}}};
    body["response_format"] = {{"type", "json_schema"}, {"json_schema", {{"name", "cat"}, {"strict", true},
                                                                         {"schema", schema}}}};
    CHECK_EQ(post(running.url("/v1/chat/completions"), body.dump()).status, 200);
    CHECK(json::parse(running.model.last().schema) == schema);

    body["response_format"] = {{"type", "text"}};
    CHECK_EQ(post(running.url("/v1/chat/completions"), body.dump()).status, 200);
    CHECK(running.model.last().schema.empty());

    body["response_format"] = {{"type", "yaml"}};
    const util::http::Response refused = post(running.url("/v1/chat/completions"), body.dump());
    CHECK_EQ(refused.status, 400);
    CHECK(parsed(refused)["error"].value("param", "") == "response_format");
    body["response_format"] = {{"type", "json_schema"}, {"json_schema", {{"schema", "an object, please"}}}};
    CHECK_EQ(post(running.url("/v1/chat/completions"), body.dump()).status, 400);
}

TEST(a_picture_is_taken_as_a_data_url_and_never_fetched) {
    if (!curl_here()) {
        return;
    }
    Calling running;
    const std::string png = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC";
    const auto with_picture = [&](const std::string& url, const std::string& role = "user") {
        return json{{"messages", json::array({json{
                        {"role", role},
                        {"content", json::array({json{{"type", "text"}, {"text", "What is this?"}},
                                                 json{{"type", "image_url"}, {"image_url", {{"url", url}}}}})}}})}};
    };
    CHECK_EQ(post(running.url("/v1/chat/completions"), with_picture("data:image/png;base64," + png).dump()).status,
             200);
    const serve::Ask asked = running.model.last();
    CHECK(asked.messages.size() == 1);
    if (asked.messages.size() == 1) {
        CHECK_EQ(asked.messages[0].content, "What is this?");
        CHECK(asked.messages[0].images.size() == 1);
        if (asked.messages[0].images.size() == 1) {
            CHECK_EQ(asked.messages[0].images[0].mime, "image/png");
            CHECK_EQ(asked.messages[0].images[0].data, png);
        }
        // Where the picture was, for the template.
        CHECK(asked.chat[0]["content"][1].value("type", "") == "media_marker");
    }

    // A picture on the web is not fetched: this machine would fetch whatever
    // anybody who can reach it named, on its network too.
    const util::http::Response web = post(running.url("/v1/chat/completions"),
                                          with_picture("https://example.com/cat.png").dump());
    CHECK_EQ(web.status, 400);
    CHECK(parsed(web)["error"].value("message", "").find("data:") != std::string::npos);
    CHECK_EQ(post(running.url("/v1/chat/completions"), with_picture("data:image/png;base64,***").dump()).status, 400);
    CHECK_EQ(post(running.url("/v1/chat/completions"), with_picture("data:text/plain;base64,aGk=").dump()).status, 400);
    CHECK_EQ(post(running.url("/v1/chat/completions"),
                  with_picture("data:image/png;base64," + png, "system").dump()).status, 400);
}

TEST(embeddings_come_back_as_floats_or_base64_from_the_model_named) {
    if (!curl_here()) {
        return;
    }
    Calling running;
    const json body{{"input", json::array({"one two three", "four"})}, {"encoding_format", "float"}};
    const util::http::Response answered = post(running.url("/v1/embeddings"), body.dump());
    CHECK_EQ(answered.status, 200);
    const json reply = parsed(answered);
    CHECK(reply.value("object", "") == "list");
    CHECK(reply.value("model", "") == "embedder.gguf");   // the one chosen for embeddings
    CHECK(reply["data"].size() == 2);
    CHECK(reply["usage"].value("prompt_tokens", 0) == 7);
    const std::vector<float> first = reply["data"][0]["embedding"].get<std::vector<float>>();
    CHECK(first.size() == 3);
    CHECK(reply["data"][1].value("index", -1) == 1);
    CHECK(running.model.last().embed);
    CHECK(running.model.last().texts.size() == 2);

    // OpenAI's own client asks for base64: the floats' bytes.
    const json b64 = parsed(post(running.url("/v1/embeddings"), json{{"input", "one two three"},
                                                                     {"encoding_format", "base64"}}.dump()));
    std::string bytes;
    CHECK(format::from_base64(b64["data"][0].value("embedding", ""), bytes));
    CHECK(bytes.size() == 3 * sizeof(float));
    if (bytes.size() == 3 * sizeof(float)) {
        std::vector<float> decoded(3);
        std::memcpy(decoded.data(), bytes.data(), bytes.size());
        CHECK(decoded == first);
    }

    // Shortened, and of unit length again.
    const json shorter = parsed(post(running.url("/v1/embeddings"), json{{"input", "one two three"},
                                                                         {"dimensions", 2}}.dump()));
    const std::vector<float> two = shorter["data"][0]["embedding"].get<std::vector<float>>();
    CHECK(two.size() == 2);
    if (two.size() == 2) {
        CHECK(std::fabs(two[0] * two[0] + two[1] * two[1] - 1.0F) < 1e-5F);
    }
    CHECK_EQ(post(running.url("/v1/embeddings"), json{{"input", "x"}, {"dimensions", 4}}.dump()).status, 400);

    // The model served may be named too; any other may not, nor may the
    // embeddings model be chatted with.
    CHECK_EQ(post(running.url("/v1/embeddings"), json{{"input", "x"}, {"model", "caller"}}.dump()).status, 200);
    CHECK_EQ(running.model.last().model, "caller.gguf");
    CHECK_EQ(post(running.url("/v1/embeddings"), json{{"input", "x"}, {"model", "text-embedding-3-small"}}.dump()).status,
             404);
    json talk = chat("hello");
    talk["model"] = "embedder.gguf";
    CHECK_EQ(post(running.url("/v1/chat/completions"), talk.dump()).status, 400);

    // Text only, and something of it.
    for (const json& input : {json(""), json::array(), json::array({1, 2, 3}), json(42)}) {
        const util::http::Response refused = post(running.url("/v1/embeddings"), json{{"input", input}}.dump());
        CHECK_EQ(refused.status, 400);
        CHECK(parsed(refused)["error"].value("param", "") == "input");
    }
    CHECK_EQ(post(running.url("/v1/embeddings"), json{{"input", "x"}, {"encoding_format", "int8"}}.dump()).status, 400);

    // Both are listed, for a client to pick each from.
    const json listed = parsed(get(running.url("/v1/models")));
    CHECK(listed["data"].size() == 2);
    CHECK_EQ(get(running.url("/v1/models/embedder")).status, 200);
}

TEST(a_body_nested_past_any_conversation_is_refused) {
    if (!curl_here()) {
        return;
    }
    Calling running;
    std::string deep = "1";
    for (int i = 0; i < 200; ++i) {
        deep = "[" + deep + "]";
    }
    const std::string body = R"({"messages":[{"role":"user","content":"hi"}],"metadata":)" + deep + "}";
    const util::http::Response refused = post(running.url("/v1/chat/completions"), body);
    CHECK_EQ(refused.status, 400);
    CHECK(parsed(refused)["error"].value("message", "").find("nests") != std::string::npos);
}

TEST(the_server_listens_on_ipv4_and_ipv6_both) {
    if (!curl_here()) {
        return;
    }
    // This machine: 127.0.0.1, and ::1 beside it where there is IPv6.
    {
        Calling running;
        CHECK_EQ(get(running.url("/v1/models")).status, 200);
        if (running.server.status().ipv6) {
            CHECK_EQ(get("http://[::1]:" + std::to_string(running.port) + "/v1/models").status, 200);
        } else {
            std::printf("      no IPv6 here: ::1 not tried\n");
        }
    }
    // The network: one socket for both kinds, IPv4 coming in on it too.
    {
        Calling running(true);
        CHECK(running.server.status().listening);
        CHECK_EQ(get(running.url("/v1/models")).status, 200);
        if (running.server.status().ipv6) {
            CHECK_EQ(get("http://[::1]:" + std::to_string(running.port) + "/v1/models").status, 200);
        }
        // From where a request came is said as a person reads it, not as
        // the IPv6 address an IPv4 client arrives on.
        json hello = chat("hi");
        hello.erase("model");
        CHECK_EQ(post(running.url("/v1/chat/completions"), hello.dump()).status, 200);
        const std::vector<serve::Record> recent = running.server.status().recent;
        CHECK(!recent.empty());
        if (!recent.empty()) {
            CHECK_EQ(recent.front().from, "127.0.0.1");
        }
    }
}

TEST(the_server_listens_nowhere_until_it_is_turned_on) {
    StandIn       model;
    serve::Server server(model);
    CHECK(!server.status().listening);
    CHECK(!server.status().enabled);

    serve::Settings off;
    CHECK(server.apply(off).empty());
    CHECK(!server.status().listening);
}

TEST(the_models_list_names_the_model_served) {
    if (!curl_here()) {
        return;
    }
    Running running;
    CHECK(running.server.status().listening);
    CHECK(running.port > 0);

    const util::http::Response listed = get(running.url("/v1/models"));
    CHECK_EQ(listed.status, 200);
    const json body = parsed(listed);
    CHECK(body.value("object", "") == "list");
    CHECK(body["data"].size() == 1);
    CHECK(body["data"][0].value("id", "") == "stand-in.gguf");
    CHECK(body["data"][0].value("object", "") == "model");
    // Readable from a page on another origin.
    CHECK_EQ(listed.header("access-control-allow-origin"), "*");

    const util::http::Response one = get(running.url("/v1/models/stand-in.gguf"));
    CHECK_EQ(one.status, 200);
    CHECK(parsed(one).value("id", "") == "stand-in.gguf");
    CHECK_EQ(get(running.url("/v1/models/gpt-4o")).status, 404);
}

TEST(a_completion_comes_back_whole_with_the_thinking_apart_and_the_usage) {
    if (!curl_here()) {
        return;
    }
    Running running;
    const util::http::Response answered =
        post(running.url("/v1/chat/completions"),
             chat("hello there", {{"temperature", 0.25}, {"top_p", 0.5}, {"seed", 42}, {"max_tokens", 50}}).dump());
    CHECK_EQ(answered.status, 200);
    const json body = parsed(answered);
    CHECK(body.value("object", "") == "chat.completion");
    CHECK(body.value("model", "") == "stand-in.gguf");
    CHECK(body.value("id", "").rfind("chatcmpl-", 0) == 0);
    const json& message = body["choices"][0]["message"];
    CHECK(message.value("role", "") == "assistant");
    CHECK(message.value("content", "") == "You said: hello there END");
    CHECK(message.value("reasoning_content", "") == "Thinking it over.");
    CHECK(body["choices"][0].value("finish_reason", "") == "stop");
    CHECK(body["usage"].value("completion_tokens", 0) == 5);
    CHECK(body["usage"].value("total_tokens", 0)
          == body["usage"].value("prompt_tokens", 0) + body["usage"].value("completion_tokens", 0));

    // What the model was asked: the conversation as sent, and the sampling.
    const serve::Ask asked = running.model.last();
    CHECK(asked.messages.size() == 2);
    CHECK(asked.messages[0].role == "system");
    CHECK(asked.messages[1].content == "hello there");
    CHECK(asked.temperature && *asked.temperature == 0.25F);
    CHECK(asked.top_p && *asked.top_p == 0.5F);
    CHECK(asked.seed && *asked.seed == 42U);
    CHECK(asked.max_tokens && *asked.max_tokens == 50);
    CHECK(!asked.raw);

    // Its settings page lists it.
    const serve::Status status = running.server.status();
    CHECK(!status.recent.empty());
    if (!status.recent.empty()) {
        CHECK_EQ(status.recent.front().status, 200);
        CHECK_EQ(status.recent.front().completion_tokens, 5);
        CHECK(status.recent.front().model == "stand-in.gguf");
    }
}

TEST(a_streamed_completion_comes_as_events_and_ends_with_done) {
    if (!curl_here()) {
        return;
    }
    Running running;
    const Events events = stream(running.url("/v1/chat/completions"),
                                 chat("stream this", {{"stream", true},
                                                      {"stream_options", {{"include_usage", true}}}}));
    CHECK_EQ(events.status, 200);
    CHECK(events.done);
    CHECK(events.data.size() > 3);
    const Told told = told_by(events);
    CHECK_EQ(told.role, "assistant");
    CHECK_EQ(told.content, "You said: stream this END");
    CHECK_EQ(told.reasoning, "Thinking it over.");
    CHECK_EQ(told.finish, "stop");
    CHECK(told.usage.value("completion_tokens", 0) == 5);
    for (const json& event : events.data) {
        CHECK(event.value("object", "") == "chat.completion.chunk");
    }
}

TEST(max_tokens_and_stop_sequences_end_the_answer_and_say_why) {
    if (!curl_here()) {
        return;
    }
    Running running;
    const json cut = parsed(post(running.url("/v1/chat/completions"),
                                 chat("one two three four", {{"max_tokens", 3}}).dump()));
    CHECK(cut["choices"][0]["message"].value("content", "") == "You said: one");
    CHECK(cut["choices"][0].value("finish_reason", "") == "length");

    // Stopped before the word, as OpenAI does, and as a stop rather than a cut.
    const json stopped = parsed(post(running.url("/v1/chat/completions"),
                                     chat("alpha beta gamma", {{"stop", json::array({"gamma", "zeta"})}}).dump()));
    CHECK(stopped["choices"][0]["message"].value("content", "") == "You said: alpha beta ");
    CHECK(stopped["choices"][0].value("finish_reason", "") == "stop");

    // And streamed, where the word arrives in pieces of its own.
    const Told streamed = told_by(stream(running.url("/v1/chat/completions"),
                                         chat("red green blue", {{"stream", true}, {"stop", "green b"}})));
    CHECK_EQ(streamed.content, "You said: red ");
    CHECK_EQ(streamed.finish, "stop");
}

TEST(a_text_completion_is_the_prompt_as_it_is) {
    if (!curl_here()) {
        return;
    }
    Running running;
    const util::http::Response answered =
        post(running.url("/v1/completions"), json{{"prompt", "Once upon a time"}, {"max_tokens", 4}}.dump());
    CHECK_EQ(answered.status, 200);
    const json body = parsed(answered);
    CHECK(body.value("object", "") == "text_completion");
    CHECK(body["choices"][0].value("text", "") == "You said: Once upon");
    CHECK(body["choices"][0].value("finish_reason", "") == "length");
    CHECK(running.model.last().raw);

    const Told streamed = told_by(stream(running.url("/v1/completions"),
                                         json{{"prompt", "The end"}, {"stream", true}}));
    CHECK_EQ(streamed.content, "You said: The end END");
}

TEST(a_wrong_key_is_refused_and_the_right_one_is_let_in) {
    if (!curl_here()) {
        return;
    }
    Running running("sk-right-key");
    const std::string body = chat("hi").dump();

    const util::http::Response none = post(running.url("/v1/chat/completions"), body);
    CHECK_EQ(none.status, 401);
    CHECK(parsed(none)["error"].value("code", "") == "invalid_api_key");
    CHECK(parsed(none)["error"].value("type", "") == "invalid_request_error");

    const util::http::Response wrong = post(running.url("/v1/chat/completions"), body, "sk-wrong-key");
    CHECK_EQ(wrong.status, 401);
    CHECK(parsed(wrong)["error"].value("code", "") == "invalid_api_key");
    CHECK_EQ(get(running.url("/v1/models"), "sk-wrong").status, 401);
    // Nearly right is wrong.
    CHECK_EQ(post(running.url("/v1/chat/completions"), body, "sk-right-ke").status, 401);
    CHECK_EQ(post(running.url("/v1/chat/completions"), body, "sk-right-keyy").status, 401);

    const util::http::Response right = post(running.url("/v1/chat/completions"), body, "sk-right-key");
    CHECK_EQ(right.status, 200);
    CHECK_EQ(get(running.url("/v1/models"), "sk-right-key").status, 200);
    // The settings page counts the refusals too.
    bool refused = false;
    for (const serve::Record& one : running.server.status().recent) {
        refused = refused || one.status == 401;
    }
    CHECK(refused);

    // Taken away, the server is open again, without starting over.
    serve::Settings open;
    open.enabled = true;
    open.port    = 0;
    CHECK(running.server.apply(open).empty());
    CHECK_EQ(running.server.status().port, running.port);
    CHECK_EQ(post(running.url("/v1/chat/completions"), body).status, 200);
}

TEST(an_open_server_needs_no_key) {
    if (!curl_here()) {
        return;
    }
    Running running;
    CHECK(!running.server.status().key);
    CHECK_EQ(post(running.url("/v1/chat/completions"), chat("open").dump()).status, 200);
    // A key nobody asked for is no reason to refuse.
    CHECK_EQ(post(running.url("/v1/chat/completions"), chat("open").dump(), "anything").status, 200);
}

TEST(a_body_too_large_is_refused_before_it_is_read) {
    if (!curl_here()) {
        return;
    }
    Running running;
    const std::string huge = chat(std::string(serve::Server::kLargestBody + 4096, 'a')).dump();
    const util::http::Response refused = post(running.url("/v1/chat/completions"), huge);
    CHECK_EQ(refused.status, 413);
    CHECK(parsed(refused)["error"].value("code", "") == "request_too_large");
    // And the model was never asked.
    CHECK(running.model.last().messages.empty());
}

TEST(a_request_for_another_model_is_refused_in_openai_shape) {
    if (!curl_here()) {
        return;
    }
    Running running;
    json body = chat("hi");
    body["model"] = "gpt-4o";
    const util::http::Response refused = post(running.url("/v1/chat/completions"), body.dump());
    CHECK_EQ(refused.status, 404);
    const json error = parsed(refused)["error"];
    CHECK(error.value("code", "") == "model_not_found");
    CHECK(error.value("param", "") == "model");
    CHECK(error.value("message", "").find("gpt-4o") != std::string::npos);

    // The file's name without .gguf is the same model, and no name is the
    // one there is.
    body["model"] = "stand-in";
    CHECK_EQ(post(running.url("/v1/chat/completions"), body.dump()).status, 200);
    body.erase("model");
    CHECK_EQ(post(running.url("/v1/chat/completions"), body.dump()).status, 200);
}

TEST(what_the_server_does_not_take_is_refused_in_openai_shape) {
    if (!curl_here()) {
        return;
    }
    Running running;
    // Nothing but the API: no other path, no files.
    for (const char* path : {"/", "/v1/embeddings", "/../../etc/passwd", "/v1/models/../../config.json"}) {
        const util::http::Response missing = get(running.url(path));
        CHECK_EQ(missing.status, 404);
        CHECK(parsed(missing)["error"].is_object());
    }
    const util::http::Response bad = post(running.url("/v1/chat/completions"), "{not json");
    CHECK_EQ(bad.status, 400);
    CHECK(parsed(bad)["error"].value("type", "") == "invalid_request_error");
    CHECK_EQ(post(running.url("/v1/chat/completions"), json{{"messages", json::array()}}.dump()).status, 400);
    // A tool's result that answers no call.
    json tool = chat("hi");
    tool["messages"].push_back(json{{"role", "tool"}, {"content", "42"}});
    CHECK_EQ(post(running.url("/v1/chat/completions"), tool.dump()).status, 400);
    CHECK_EQ(post(running.url("/v1/chat/completions"), chat("hi", {{"n", 2}}).dump()).status, 400);
    CHECK_EQ(post(running.url("/v1/chat/completions"), chat("hi", {{"temperature", "hot"}}).dump()).status, 400);

    // A browser's preflight is answered, with what the page may send.
    util::http::Request preflight;
    preflight.method = "OPTIONS";
    preflight.url    = running.url("/v1/chat/completions");
    preflight.headers.push_back({"Origin", "http://example.test"});
    preflight.headers.push_back({"Access-Control-Request-Method", "POST"});
    preflight.headers.push_back({"Access-Control-Request-Headers", "authorization, content-type"});
    const util::http::Response answered = util::http::send(preflight);
    CHECK(answered.status == 204 || answered.status == 200);
    CHECK_EQ(answered.header("access-control-allow-origin"), "*");
    CHECK(answered.header("access-control-allow-headers").find("authorization") != std::string::npos);
}

TEST(a_port_already_taken_is_said) {
    if (!curl_here()) {
        return;
    }
    Running first;
    StandIn       model;
    serve::Server second(model);
    serve::Settings same;
    same.enabled = true;
    same.port    = first.port;
    const std::string error = second.apply(same);
    CHECK(error.find("in use") != std::string::npos);
    CHECK(!second.status().listening);
    CHECK_EQ(second.status().error, error);
    // The first is still the one answering.
    CHECK_EQ(get(first.url("/v1/models")).status, 200);
}

TEST(turning_it_off_ends_a_request_in_flight_at_once) {
    if (!curl_here()) {
        return;
    }
    // A model that never gets the memory it waits for, until it is told to
    // stop: what a request behind a build's agents looks like.
    class Waiting final : public serve::Backend {
    public:
        std::string model() const override { return "waiting.gguf"; }
        serve::Answer answer(const serve::Ask&, const serve::Sink& sink) override {
            sink.waiting("memory");
            for (int i = 0; i < 3000 && !sink.cancel(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            serve::Answer out;
            out.error = "stopped";
            return out;
        }
    };
    Waiting         model;
    serve::Server   server(model);
    serve::Settings on;
    on.enabled = true;
    server.apply(on);
    const std::string url = "http://127.0.0.1:" + std::to_string(server.status().port) + "/v1/chat/completions";

    json asking = chat("anyone there?");
    asking.erase("model");
    util::http::Response answered;
    std::thread          client([&] { answered = post(url, asking.dump()); });
    // What it waits for is said while it waits.
    bool said = false;
    for (int i = 0; i < 200 && !said; ++i) {
        said = server.status().waiting == "memory";
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK(said);
    CHECK(server.status().answering);

    const auto      began = std::chrono::steady_clock::now();
    serve::Settings off;
    server.apply(off);
    client.join();
    CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(3));
    CHECK_EQ(answered.status, 503);
    CHECK(parsed(answered)["error"].value("code", "") == "server_stopped");
    CHECK(!server.status().listening);
    // And the thread that answers is free again at once.
    server.stop();
    CHECK(!server.status().answering);
}

TEST(a_client_that_never_finishes_holds_up_nothing) {
    if (!curl_here()) {
        return;
    }
    // A request that arrives a byte a second: the thread reading it does not
    // look up until the next byte comes, so neither turning the server off,
    // nor stopping it, nor the model going may wait for it.
    TempDir                     dir;
    const std::filesystem::path body = dir.path() / "body.json";
    {
        std::ofstream out(body, std::ios::binary);
        out << chat(std::string(std::size_t{1} << 20, 'a')).dump();
    }
    auto            model  = std::make_unique<StandIn>();
    auto            server = std::make_unique<serve::Server>(*model);
    serve::Settings on;
    on.enabled = true;
    server->apply(on);
    const std::string url = "http://127.0.0.1:" + std::to_string(server->status().port) + "/v1/chat/completions";

    util::Subprocess slow;
    std::string      error;
    CHECK(slow.start({"curl", "-s", "--limit-rate", "1", "-H", "Content-Type: application/json",
                      "--data-binary", "@" + body.string(), url},
                     {}, {}, error));
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    CHECK(slow.running());
    CHECK(server->status().recent.empty());   // still arriving, not answered

    const auto      began = std::chrono::steady_clock::now();
    serve::Settings off;
    server->apply(off);
    CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(2));
    server->stop();
    CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(2));
    // Gone, both, with the request still arriving.
    server.reset();
    model.reset();
    CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(2));
    slow.terminate();
    slow.wait();
}

TEST(the_status_is_written_for_the_page_with_its_addresses) {
    serve::Status status;
    status.enabled   = true;
    status.listening = true;
    status.port      = 8338;
    status.network   = true;
    status.lan       = "192.168.1.20";
    status.model     = "stand-in.gguf";
    serve::Record record;
    record.model             = "stand-in.gguf";
    record.prompt_tokens     = 12;
    record.completion_tokens = 40;
    record.ms                = 1500;
    status.recent.push_back(record);
    const json out = serve::status_json(status);
    CHECK(out["addresses"].value("local", "") == "http://localhost:8338/v1");
    CHECK(out["addresses"].value("network", "") == "http://192.168.1.20:8338/v1");
    CHECK(out["recent"][0].value("completion", 0) == 40);
    CHECK(!out["addresses"].contains("local6"));

    // Both kinds of address, where it listens on both.
    status.ipv6 = true;
    status.lan6 = "fd00::20";
    const json both = serve::status_json(status);
    CHECK(both["addresses"].value("local6", "") == "http://[::1]:8338/v1");
    CHECK(both["addresses"].value("network6", "") == "http://[fd00::20]:8338/v1");
    CHECK(both.value("lan6", "") == "fd00::20");

    status.network = false;
    CHECK(!serve::status_json(status)["addresses"].contains("network"));
    CHECK(!serve::status_json(status)["addresses"].contains("network6"));
    // Over HTTPS, every address says so.
    status.https       = true;
    status.network     = true;
    status.fingerprint = "AB:CD";
    const json secure = serve::status_json(status);
    CHECK(secure["addresses"].value("local", "") == "https://localhost:8338/v1");
    CHECK(secure["addresses"].value("network6", "") == "https://[fd00::20]:8338/v1");
    CHECK(secure.value("https", false));
    CHECK(secure.value("fingerprint", "") == "AB:CD");

    status.listening = false;
    CHECK(serve::status_json(status)["addresses"].empty());
    CHECK(!serve::status_json(status).value("https", true));
}

TEST(the_server_settings_survive_the_config_file) {
    Config config;
    CHECK(!config.server.enabled);
    CHECK_EQ(config.server.port, ServerConfig::kDefaultPort);
    CHECK(config.server.port != 1234);   // LM Studio's
    config.server.enabled    = true;
    config.server.model      = "LFM2.5-1.2B-Instruct-Q8_0.gguf";
    config.server.embeddings = "nomic-embed-text-v1.5.Q8_0.gguf";
    config.server.port       = 9001;
    config.server.network    = true;
    config.server.api_key    = "sk-local";
    config.server.https      = true;
    config.server.certificate     = "~/certs/server.crt";
    config.server.certificate_key = "~/certs/server.key";
    std::vector<std::string> warnings;
    const Config back = config_from_json_text(config_to_json_text(config), warnings);
    CHECK(back.server.enabled);
    CHECK_EQ(back.server.model, "LFM2.5-1.2B-Instruct-Q8_0.gguf");
    CHECK_EQ(back.server.embeddings, "nomic-embed-text-v1.5.Q8_0.gguf");
    CHECK(back.server.provider.empty());
    CHECK_EQ(back.server.port, 9001);
    CHECK(back.server.network);
    CHECK_EQ(back.server.api_key, "sk-local");
    CHECK(back.server.https);
    CHECK_EQ(back.server.certificate, "~/certs/server.crt");
    CHECK_EQ(back.server.certificate_key, "~/certs/server.key");
    CHECK(!Config{}.server.https);   // off until somebody turns it on

    config.server.provider = "anthropic";
    config.server.model    = "claude-opus-5-5";
    const Config remote = config_from_json_text(config_to_json_text(config), warnings);
    CHECK_EQ(remote.server.provider, "anthropic");
    CHECK_EQ(remote.server.model, "claude-opus-5-5");

    // A port no machine has is the default, and said.
    warnings.clear();
    const Config odd = config_from_json_text(R"({"server": {"port": 70000}})", warnings);
    CHECK_EQ(odd.server.port, ServerConfig::kDefaultPort);
    CHECK(!warnings.empty());
}

TEST(a_providers_model_reaches_other_computers_only_with_a_key) {
    // Every request to it is the person's credit spent: with the network on
    // and no key, the server stays on this machine. No request is made, so
    // the provider need not be there.
    TempDir              dir;
    const ScopedDataHome scoped(dir.path());
    Config               config;
    Provider             far;
    far.id       = "far";
    far.name     = "Far";
    far.kind     = "openai";
    far.base_url = "http://127.0.0.1:9/v1";
    config.providers.push_back(far);
    config.server.enabled  = true;
    config.server.port     = 0;
    config.server.network  = true;
    config.server.provider = "far";
    config.server.model    = "far-model";
    AppState state;
    Engine   engine(config, state, {});
    engine.start();
    CHECK(engine.server_status().listening);
    CHECK(!engine.server_status().network);
    CHECK_EQ(engine.server_status().model, "far-model");

    config.server.api_key = "sk-local";
    engine.apply_config(config);
    CHECK(engine.server_status().listening);
    CHECK(engine.server_status().network);
    engine.stop();
}

// --- HTTPS -----------------------------------------------------------------------

namespace {

/// curl, told to trust `certificate` and nothing else, asking for `url`:
/// what it printed and how it ended. The curl on the machine, as another
/// program calling the server would use it.
util::Ran curl_trusting(const std::string& certificate, const std::string& url) {
    std::vector<std::string> argv{"curl", "--silent", "--show-error", "--max-time", "20", "--cacert", certificate};
#if defined(_WIN32)
    // Schannel, under the curl Windows ships, asks whoever signed a
    // certificate whether it was revoked, and one that signed itself has
    // nobody to ask.
    argv.emplace_back("--ssl-no-revoke");
#endif
    argv.push_back(url);
    return util::run(argv, {}, {}, 30);
}

/// A server over HTTPS on a free port, answered by the stand-in: with the
/// certificate and key named, or Crucible's own kept in `own_dir`.
struct Secure {
    StandIn       model;
    serve::Server server{model};
    int           port = 0;
    std::string   error;

    explicit Secure(const std::filesystem::path& own_dir, const std::filesystem::path& certificate = {},
                    const std::filesystem::path& key = {}) {
        serve::Settings settings;
        settings.enabled         = true;
        settings.port            = 0;
        settings.https           = true;
        settings.certificate     = certificate;
        settings.certificate_key = key;
        settings.own_dir         = own_dir;
        error = server.apply(settings);
        port  = server.status().port;
    }

    std::string url(const std::string& path, const std::string& host = "localhost") const {
        return "https://" + host + ":" + std::to_string(port) + path;
    }
};

void write_text(const std::filesystem::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary);
    out << text;
}

/// The certificate in `pem` as DER: the base64 between its lines, decoded.
std::string der_of(const std::string& pem) {
    std::string base64;
    std::istringstream lines(pem);
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("-----", 0) != 0) {
            base64 += line;
        }
    }
    std::string der;
    return format::from_base64(base64, der) ? der : std::string();
}

bool holds(const std::vector<std::string>& list, const std::string& name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}

}  // namespace

TEST(https_answers_a_client_that_trusts_the_certificate_crucible_made) {
    if (!curl_here()) {
        return;
    }
    TempDir      dir;
    const Secure secure(dir.path() / "https");
    CHECK_EQ(secure.error, "");
    const serve::Status status = secure.server.status();
    CHECK(status.listening);
    CHECK(status.https);
    CHECK(status.own_certificate);
    CHECK_EQ(status.fingerprint.size(), std::size_t{95});   // 32 pairs and the colons between
    CHECK(std::filesystem::exists(status.certificate));

    const util::Ran listed = curl_trusting(status.certificate, secure.url("/v1/models"));
    CHECK_EQ(listed.status, 0);
    const json body = json::parse(listed.output, nullptr, false);
    CHECK(body.is_object() && body.value("object", "") == "list");
    CHECK(body.is_object() && body["data"][0].value("id", "") == "stand-in.gguf");
    if (listed.status != 0) {
        std::printf("      curl said: %s\n", listed.output.c_str());
    }
#if !defined(_WIN32)
    // By its address as well, which the certificate names beside the name.
    // Not where Schannel checks a certificate the client was handed, since
    // it reads only the names in it.
    CHECK_EQ(curl_trusting(status.certificate, secure.url("/v1/models", "127.0.0.1")).status, 0);
#endif
    // And a chat answered over it, streamed as it would be in the clear. The
    // body from a file, which no command line has to quote.
    write_text(dir.path() / "asked.json", chat("over https", json{{"stream", true}}).dump());
    std::vector<std::string> argv{"curl", "--silent", "--show-error", "--max-time", "20", "--cacert",
                                  status.certificate, "-H", "Content-Type: application/json",
                                  "--data-binary", "@" + (dir.path() / "asked.json").string()};
#if defined(_WIN32)
    argv.emplace_back("--ssl-no-revoke");
#endif
    argv.push_back(secure.url("/v1/chat/completions"));
    const util::Ran streamed = util::run(argv, {}, {}, 30);
    CHECK_EQ(streamed.status, 0);
    CHECK(streamed.output.find("over") != std::string::npos);
    CHECK(streamed.output.find("data: [DONE]") != std::string::npos);

    // The page is given the https addresses, and what a client checks.
    const json page = serve::status_json(status);
    CHECK(page["addresses"].value("local", "").rfind("https://localhost:", 0) == 0);
    CHECK(page.value("https", false));
    CHECK_EQ(page.value("fingerprint", ""), status.fingerprint);
    CHECK(page.value("own_certificate", false));
}

TEST(plain_http_to_the_https_port_fails_at_once_and_harms_nothing) {
    if (!curl_here()) {
        return;
    }
    TempDir      dir;
    const Secure secure(dir.path() / "https");
    CHECK(secure.server.status().listening);

    const auto          began = std::chrono::steady_clock::now();
    util::http::Request request;
    request.url             = "http://127.0.0.1:" + std::to_string(secure.port) + "/v1/models";
    request.timeout_seconds = 20;
    const util::http::Response plain = util::http::send(request);
    CHECK_EQ(plain.status, 0);   // no HTTP answer at all: the handshake was not one
    CHECK(!plain.error.empty());
    CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(10));

    // Still there, and still answering the way it should.
    CHECK(secure.server.status().listening);
    CHECK_EQ(curl_trusting(secure.server.status().certificate, secure.url("/v1/models")).status, 0);
}

TEST(a_key_that_is_not_the_certificates_is_refused_and_said) {
    TempDir                 dir;
    const serve::tls::Names here{{"localhost"}, {"127.0.0.1", "::1"}};
    serve::tls::Pem         one;
    serve::tls::Pem         other;
    std::string             error;
    CHECK(serve::tls::make(here, 30, one, error));
    CHECK(serve::tls::make(here, 30, other, error));
    write_text(dir.path() / "server.crt", one.certificate);
    write_text(dir.path() / "server.key", other.key);

    Secure mismatched(dir.path() / "own", dir.path() / "server.crt", dir.path() / "server.key");
    CHECK_EQ(mismatched.error, "The key does not match the certificate");
    CHECK(!mismatched.server.status().listening);
    CHECK_EQ(mismatched.server.status().error, mismatched.error);
    // Nothing of Crucible's own was made in its place.
    CHECK(!std::filesystem::exists(dir.path() / "own"));

    // The right key, after the certificate in one file, serves.
    write_text(dir.path() / "both.pem", one.certificate + one.key);
    const Secure matched(dir.path() / "own", dir.path() / "both.pem");
    CHECK_EQ(matched.error, "");
    CHECK(matched.server.status().https);
    CHECK(!matched.server.status().own_certificate);
    CHECK_EQ(matched.server.status().fingerprint, serve::tls::fingerprint(one.certificate));
    if (curl_here()) {
        write_text(dir.path() / "trust.crt", one.certificate);
        CHECK_EQ(curl_trusting((dir.path() / "trust.crt").string(), matched.url("/v1/models")).status, 0);
    }
}

TEST(what_will_not_serve_over_https_says_why) {
    const serve::tls::Names here{{"localhost"}, {"127.0.0.1"}};
    serve::tls::Pem         good;
    serve::tls::Pem         other;
    std::string             error;
    CHECK(serve::tls::make(here, 30, good, error));
    CHECK(serve::tls::make(here, 30, other, error));

    serve::tls::Pem out;
    CHECK_EQ(serve::tls::prepare({"not a certificate", good.key}, out), "Not a certificate");
    CHECK_EQ(serve::tls::prepare({good.certificate, "not a key"}, out), "Not a key");
    CHECK_EQ(serve::tls::prepare({good.certificate, other.key}, out), "The key does not match the certificate");
    serve::tls::Pem old;
    CHECK(serve::tls::make(here, -2, old, error));   // good until two days ago
    CHECK_EQ(serve::tls::prepare(old, out), "The certificate has expired");

    // DER is taken as well as PEM, and written as PEM for the listener.
    const std::string der = der_of(good.certificate);
    CHECK(!der.empty());
    CHECK_EQ(serve::tls::prepare({der, good.key}, out), "");
    CHECK_EQ(out.certificate, good.certificate);
    CHECK_EQ(serve::tls::fingerprint(der), serve::tls::fingerprint(good.certificate));

    // Files that are not there, said as such.
    TempDir              dir;
    serve::tls::Identity identity;
    CHECK(!serve::tls::load(dir.path() / "missing.crt", {}, dir.path() / "own", identity, error));
    CHECK_EQ(error, "Cannot read the certificate");
    write_text(dir.path() / "server.crt", good.certificate);
    CHECK(!serve::tls::load(dir.path() / "server.crt", dir.path() / "missing.key", dir.path() / "own", identity, error));
    CHECK_EQ(error, "Cannot read the key");
}

TEST(the_certificate_crucible_makes_is_kept_and_used_again) {
    TempDir              dir;
    serve::tls::Identity first;
    serve::tls::Identity again;
    std::string          error;
    CHECK(serve::tls::own(dir.path(), false, first, error));
    CHECK(first.own);
    CHECK(std::filesystem::exists(dir.path() / "certificate.pem"));
    CHECK(std::filesystem::exists(dir.path() / "key.pem"));
    CHECK(serve::tls::own(dir.path(), false, again, error));
    CHECK(first.pem == again.pem);
    CHECK_EQ(again.fingerprint, first.fingerprint);

    // For this machine, by name and by address, and good for as long as
    // Apple's systems take one.
    CHECK(holds(first.names, "localhost"));
    CHECK(holds(first.names, "127.0.0.1"));
    CHECK(holds(first.names, "::1"));
    const std::time_t now   = std::time(nullptr);
    const std::time_t until = serve::tls::expires(first.pem.certificate);
    CHECK(until > now + std::time_t{serve::tls::kOwnDays - 1} * 86400);
    CHECK(until <= now + std::time_t{serve::tls::kOwnDays} * 86400 + 60);
#if !defined(_WIN32)
    // The key is this user's alone.
    const auto perms = std::filesystem::status(dir.path() / "key.pem").permissions();
    CHECK((perms & (std::filesystem::perms::group_all | std::filesystem::perms::others_all))
          == std::filesystem::perms::none);
#endif

    // A server started on it again, as after a restart, shows the same.
    {
        const Secure secure(dir.path());
        CHECK_EQ(secure.error, "");
        CHECK_EQ(secure.server.status().fingerprint, first.fingerprint);
    }

    // Asked for a new one, it makes another; one near its end is made again
    // by itself, so a client is never shown one that ran out.
    serve::tls::Identity renewed;
    CHECK(serve::tls::own(dir.path(), true, renewed, error));
    CHECK(renewed.fingerprint != first.fingerprint);
    serve::tls::Pem ending;
    CHECK(serve::tls::make(serve::tls::this_machine(), serve::tls::kRenewDays - 1, ending, error));
    write_text(dir.path() / "certificate.pem", ending.certificate);
    write_text(dir.path() / "key.pem", ending.key);
    serve::tls::Identity replaced;
    CHECK(serve::tls::own(dir.path(), false, replaced, error));
    CHECK(replaced.fingerprint != serve::tls::fingerprint(ending.certificate));
    CHECK(serve::tls::expires(replaced.pem.certificate) > now + std::time_t{serve::tls::kRenewDays} * 86400);
}

TEST(the_engine_keeps_its_certificate_with_its_data) {
    TempDir              dir;
    const ScopedDataHome scoped(dir.path());
    Config               config;
    config.server.enabled = true;
    config.server.port    = 0;
    config.server.https   = true;
    AppState state;
    Engine   engine(config, state, {});
    engine.start();
    const serve::Status status = engine.server_status();
    CHECK(status.listening);
    CHECK(status.https);
    CHECK(status.own_certificate);
    CHECK(std::filesystem::path(status.certificate).parent_path() == paths::https_dir());
    CHECK(paths::https_dir().string().rfind(dir.path().string(), 0) == 0);

    // Off again, the same server over HTTP.
    config.server.https = false;
    engine.apply_config(config);
    CHECK(engine.server_status().listening);
    CHECK(!engine.server_status().https);
    CHECK(serve::status_json(engine.server_status())["addresses"].value("local", "").rfind("http://", 0) == 0);
    engine.stop();
}

TEST(the_engine_serves_a_real_model) {
    // Only where a model is named: CRUCIBLE_TEST_MODEL, a small GGUF, with a
    // runtime installed that loads it. The same requests, answered by the
    // engine from the model held as a seat holds one.
    const char* named = std::getenv("CRUCIBLE_TEST_MODEL");
    if (named == nullptr || *named == '\0' || !curl_here()) {
        return;
    }
    // The engine keeps its log and its spending in a folder of its own here,
    // with the runtimes of whoever runs this.
    const std::filesystem::path runtimes = paths::runtimes_dir();
    TempDir                     dir;
    const ScopedDataHome        scoped(dir.path());
    std::error_code             ec;
    std::filesystem::create_directories(paths::data_dir(), ec);
    std::filesystem::create_directory_symlink(runtimes, paths::runtimes_dir(), ec);
    if (ec) {
        std::printf("      skipped: the runtimes could not be linked in (%s)\n", ec.message().c_str());
        return;
    }

    const std::filesystem::path file(named);
    Config config;
    config.models_dir       = file.parent_path().string();
    config.defaults.n_ctx   = 2048;
    config.server.enabled   = true;
    config.server.port      = 0;
    config.server.model     = file.string();
    config.resolve_models();
    AppState state;
    Engine   engine(config, state, {});
    engine.start();
    const int port = engine.server_status().port;
    CHECK(engine.server_status().listening);
    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    const std::string id   = file.filename().string();

    const json listed = parsed(get(base + "/v1/models"));
    CHECK(listed["data"].size() == 1);
    CHECK(listed["data"][0].value("id", "") == id);

    const json question{{"model", id},
                        {"messages", json::array({json{{"role", "user"},
                                                       {"content", "What is the capital of France? One word."}}})},
                        {"temperature", 0},
                        {"max_tokens", 24}};
    const util::http::Response answered = post(base + "/v1/chat/completions", question.dump());
    CHECK_EQ(answered.status, 200);
    const json body = parsed(answered);
    const std::string content = body["choices"][0]["message"].value("content", "");
    CHECK(content.find("Paris") != std::string::npos);
    CHECK(body["usage"].value("prompt_tokens", 0) > 0);
    CHECK(body["usage"].value("completion_tokens", 0) > 0);
    std::printf("      answered: %s\n", content.c_str());

    json streamed = question;
    streamed["stream"] = true;
    const Told told    = told_by(stream(base + "/v1/chat/completions", streamed));
    CHECK(told.content.find("Paris") != std::string::npos);
    CHECK(!told.finish.empty());

    json other = question;
    other["model"] = "gpt-4o";
    CHECK_EQ(post(base + "/v1/chat/completions", other.dump()).status, 404);

    // A tool, written into the prompt by the model's own template and its
    // call read back out of the answer.
    json weather = json{{"model", id},
                        {"messages", json::array({json{{"role", "user"},
                                                       {"content", "What is the weather in Paris? Use the tool."}}})},
                        {"tools", json::array({weather_tool()})},
                        {"temperature", 0},
                        {"max_tokens", 128}};
    const json called = parsed(post(base + "/v1/chat/completions", weather.dump()));
    const json calls  = called["choices"][0]["message"].value("tool_calls", json::array());
    CHECK(!calls.empty());
    if (!calls.empty()) {
        CHECK(calls[0]["function"].value("name", "") == "get_weather");
        const json arguments = json::parse(calls[0]["function"].value("arguments", ""), nullptr, false);
        CHECK(arguments.is_object() && arguments.contains("city"));
        std::printf("      called: %s\n", calls[0]["function"].dump().c_str());
    }
    CHECK(called["choices"][0].value("finish_reason", "") == "tool_calls");

    // JSON, held to by a grammar.
    json cat = question;
    cat["messages"][0]["content"] = "Describe a cat with a name and an age, as JSON.";
    cat["response_format"]        = {{"type", "json_object"}};
    cat["max_tokens"]             = 96;
    const std::string described =
        parsed(post(base + "/v1/chat/completions", cat.dump()))["choices"][0]["message"].value("content", "");
    CHECK(json::parse(described, nullptr, false).is_object());
    std::printf("      as JSON: %s\n", described.c_str());

    // Embeddings, from the model served.
    const json embedded = parsed(post(base + "/v1/embeddings", json{{"input", json::array({"a cat", "a dog"})}}.dump()));
    CHECK(embedded["data"].size() == 2);
    CHECK(!embedded["data"][0]["embedding"].empty());

    // Held while it answered and let go after: the next seat may have it.
    CHECK(engine.server_status().recent.size() >= 3);

    // The same file as the delegator, which routes in a context of 1024
    // tokens and is served in one of its own as long as a model from the
    // folder would have: a conversation longer than routing's is answered.
    config.router.model  = file.string();
    config.router.n_ctx  = 1024;
    config.server.model  = "";
    config.resolve_models();
    engine.apply_config(config);
    std::string filler;
    for (int i = 0; i < 700; ++i) {
        filler += "item" + std::to_string(i) + " ";
    }
    const json longer{{"messages", json::array({json{{"role", "user"},
                                                     {"content", "A list: " + filler + ". The secret word is PELICAN."}},
                                                json{{"role", "assistant"}, {"content", "Noted."}},
                                                json{{"role", "user"}, {"content", "The secret word? One word."}}})},
                      {"temperature", 0},
                      {"max_tokens", 8}};
    const json kept = parsed(post(base + "/v1/chat/completions", longer.dump()));
    CHECK(kept["usage"].value("prompt_tokens", 0) > 1024);
    CHECK(kept["choices"][0]["message"].value("content", "").find("PELICAN") != std::string::npos);
    std::printf("      the delegator, %d tokens in: %s\n", kept["usage"].value("prompt_tokens", 0),
                kept["choices"][0]["message"].value("content", "").c_str());
    engine.stop();
    CHECK(!engine.server_status().listening);
}
