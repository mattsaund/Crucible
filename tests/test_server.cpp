// SPDX-License-Identifier: MIT
//
// The API server, called the way another program calls it: over HTTP, on a
// port of its own, with the app's own client (curl underneath).
//
// The server is asked of a stand-in model -- one that says what it was told,
// with a line of thinking first -- so every way a request can go is one a
// test can walk in a moment and with no weights: the models list, an answer
// whole and streamed, a key wrong and right, an open server, a body too
// large, a model that is not the one served, stop sequences, a port already
// taken. Where CRUCIBLE_TEST_MODEL names a small GGUF, the same requests go
// through the engine to that model as well.
#include "test_helpers.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include "crucible/engine/engine.hpp"
#include "crucible/serve/server.hpp"
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

}  // namespace

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

    status.network = false;
    CHECK(!serve::status_json(status)["addresses"].contains("network"));
    status.listening = false;
    CHECK(serve::status_json(status)["addresses"].empty());
}

TEST(the_server_settings_survive_the_config_file) {
    Config config;
    CHECK(!config.server.enabled);
    CHECK_EQ(config.server.port, ServerConfig::kDefaultPort);
    CHECK(config.server.port != 1234);   // LM Studio's
    config.server.enabled = true;
    config.server.model   = "LFM2.5-1.2B-Instruct-Q8_0.gguf";
    config.server.port    = 9001;
    config.server.network = true;
    config.server.api_key = "sk-local";
    std::vector<std::string> warnings;
    const Config back = config_from_json_text(config_to_json_text(config), warnings);
    CHECK(back.server.enabled);
    CHECK_EQ(back.server.model, "LFM2.5-1.2B-Instruct-Q8_0.gguf");
    CHECK_EQ(back.server.port, 9001);
    CHECK(back.server.network);
    CHECK_EQ(back.server.api_key, "sk-local");

    // A port no machine has is the default, and said.
    warnings.clear();
    const Config odd = config_from_json_text(R"({"server": {"port": 70000}})", warnings);
    CHECK_EQ(odd.server.port, ServerConfig::kDefaultPort);
    CHECK(!warnings.empty());
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

    // Held while it answered and let go after: the next seat may have it.
    CHECK(engine.server_status().recent.size() >= 3);
    engine.stop();
    CHECK(!engine.server_status().listening);
}
