// SPDX-License-Identifier: MIT
//
// The request file curl is handed, and the line it answers with.
//
// Nothing here touches the network. What can be wrong without one is the
// quoting -- a header value has to come out of curl's configuration parser as
// the bytes that went in -- and the one line that carries the status back. The
// first is where a secret would leak or a request would split in two; the
// second is where a 401 would be mistaken for an answer.
#include "test_helpers.hpp"

#include "crucible/util/http.hpp"

namespace {

using crucible::util::http::Request;
using crucible::util::http::Response;
using crucible::util::http::detail::curl_config;
using crucible::util::http::detail::parse_headers;
using crucible::util::http::detail::status_from_marker;

bool has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

TEST(a_request_names_its_url_method_and_headers) {
    Request request;
    request.method  = "POST";
    request.url     = "https://example.test/v1/messages";
    request.headers = {{"x-api-key", "secret"}, {"content-type", "application/json"}};

    const std::string config = curl_config(request, "/tmp/body", /*streaming=*/false);
    CHECK(has(config, "request = \"POST\""));
    CHECK(has(config, "url = \"https://example.test/v1/messages\""));
    CHECK(has(config, "header = \"x-api-key: secret\""));
    CHECK(has(config, "header = \"content-type: application/json\""));
    CHECK(has(config, "data-binary = \"@/tmp/body\""));
}

TEST(a_request_with_no_body_sends_none) {
    Request request;
    request.url = "https://example.test/";
    CHECK(!has(curl_config(request, {}, false), "data-binary"));
}

TEST(quotes_and_backslashes_survive_the_configuration_file) {
    // curl reads the value back through its own unescaping, so what is
    // written has to be escaped once and exactly once.
    Request request;
    request.url     = "https://example.test/";
    request.headers = {{"x-note", "say \"hi\" to C:\\path"}};
    CHECK(has(curl_config(request, {}, false),
              "header = \"x-note: say \\\"hi\\\" to C:\\\\path\""));
}

TEST(a_line_break_cannot_smuggle_a_second_header) {
    // The classic injection: a value that ends its own line and starts
    // another. Dropped, so the header that arrives is one header.
    Request request;
    request.url     = "https://example.test/";
    request.headers = {{"x-note", "fine\r\nx-admin: yes"}};

    const std::string config = curl_config(request, {}, false);
    CHECK(has(config, "header = \"x-note: finex-admin: yes\""));
    CHECK(!has(config, "\nx-admin"));
}

TEST(only_a_stream_asks_curl_not_to_buffer) {
    Request request;
    request.url = "https://example.test/";
    CHECK(has(curl_config(request, {}, /*streaming=*/true), "no-buffer"));
    CHECK(!has(curl_config(request, {}, /*streaming=*/false), "no-buffer"));
}

TEST(no_ceiling_means_no_max_time) {
    // A model answering for five minutes is not a hung request.
    Request request;
    request.url             = "https://example.test/";
    request.timeout_seconds = 0;
    const std::string config = curl_config(request, {}, true);
    CHECK(!has(config, "max-time"));
    CHECK(has(config, "connect-timeout"));
}

TEST(the_status_comes_back_on_its_own_line) {
    CHECK_EQ(status_from_marker("[[crucible-http-status 200]]"), 200);
    CHECK_EQ(status_from_marker("[[crucible-http-status 401]]"), 401);
    // Not that line: part of a body, or a line that merely starts like it.
    CHECK_EQ(status_from_marker("{\"ok\":true}"), -1);
    CHECK_EQ(status_from_marker("[[crucible-http-status 200"), -1);
    CHECK_EQ(status_from_marker("[[crucible-http-status abc]]"), -1);
    CHECK_EQ(status_from_marker(""), -1);
}

TEST(a_refusal_is_reported_with_what_the_server_said) {
    // The body of a 401 is the sentence that says which key was wrong, and it
    // is the first thing anybody debugging a provider needs to read.
    Response response;
    response.status = 401;
    response.body   = "{\"error\":\"invalid x-api-key\"}\nmore";
    CHECK(!response.ok());
    CHECK_EQ(response.reason(), std::string("HTTP 401: {\"error\":\"invalid x-api-key\"}"));

    Response dropped;
    dropped.error = "curl: (6) Could not resolve host: nowhere.test";
    CHECK(!dropped.ok());
    CHECK_EQ(dropped.reason(), dropped.error);

    Response fine;
    fine.status = 200;
    CHECK(fine.ok());
}

TEST(the_headers_are_the_answers_not_a_redirects) {
    // A provider's rate limits arrive as headers. With a redirect on the way
    // curl writes a block per response, and only the last is the answer's.
    const auto headers = parse_headers(
        "HTTP/1.1 301 Moved\r\nLocation: https://example.test/b\r\nx-ratelimit-limit-requests: 1\r\n\r\n"
        "HTTP/2 200\r\nContent-Type: text/event-stream\r\nX-RateLimit-Remaining-Requests:  49\r\n\r\n");
    Response response;
    response.headers = headers;
    CHECK_EQ(response.header("x-ratelimit-remaining-requests"), std::string("49"));
    CHECK_EQ(response.header("content-type"), std::string("text/event-stream"));
    CHECK(response.header("x-ratelimit-limit-requests").empty());
    CHECK(response.header("location").empty());
}

TEST(curl_is_told_where_to_put_the_headers_only_when_asked) {
    Request request;
    request.url = "https://example.test/";
    CHECK(has(curl_config(request, {}, true, "/tmp/h"), "dump-header = \"/tmp/h\""));
    CHECK(!has(curl_config(request, {}, true), "dump-header"));
}
