// SPDX-License-Identifier: MIT
//
// See web_search.hpp.
#include "crucible/tools/web_search.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

#include <nlohmann/json.hpp>

#include "crucible/util/http.hpp"

namespace crucible::tools {
namespace {

using json = nlohmann::json;

/// Percent-encode everything that is not unreserved, per RFC 3986.
///
/// A query goes into a URL, and a query is whatever the model wrote. Encoding
/// it is not a nicety: an unencoded '&' turns one parameter into two, and an
/// unencoded newline is a request-splitting bug.
std::string percent_encode(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((std::isalnum(byte) != 0) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            out += '%';
            out += kHex[byte >> 4];
            out += kHex[byte & 0x0F];
        }
    }
    return out;
}

/// Strip HTML tags and collapse whitespace.
///
/// Wikipedia marks the matched words in its snippets with <span> elements, and
/// an expert reading "<span class=\"searchmatch\">Paris</span>" is being given
/// markup to reason about instead of a sentence.
std::string plain_text(std::string_view html) {
    std::string out;
    out.reserve(html.size());
    bool in_tag = false;
    bool spaced = true;  // suppresses leading whitespace
    for (const char c : html) {
        if (c == '<') {
            in_tag = true;
            continue;
        }
        if (c == '>') {
            in_tag = false;
            continue;
        }
        if (in_tag) {
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            if (!spaced) {
                out += ' ';
                spaced = true;
            }
            continue;
        }
        out += c;
        spaced = false;
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

/// A string field, or empty if it is absent or is not a string.
std::string field(const json& object, const char* name) {
    if (!object.is_object()) {
        return {};
    }
    const auto found = object.find(name);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

std::vector<SearchResult> parse_wikipedia(const json& body, int max_results) {
    std::vector<SearchResult> results;
    const auto query = body.find("query");
    if (query == body.end() || !query->is_object()) {
        return results;
    }
    const auto hits = query->find("search");
    if (hits == query->end() || !hits->is_array()) {
        return results;
    }
    for (const json& hit : *hits) {
        if (results.size() >= static_cast<std::size_t>(std::max(1, max_results))) {
            break;
        }
        const std::string title = field(hit, "title");
        if (title.empty()) {
            continue;
        }
        results.push_back({title,
                           "https://en.wikipedia.org/wiki/" + percent_encode(title),
                           plain_text(field(hit, "snippet"))});
    }
    return results;
}

std::vector<SearchResult> parse_searxng(const json& body, int max_results) {
    std::vector<SearchResult> results;
    const auto hits = body.find("results");
    if (hits == body.end() || !hits->is_array()) {
        return results;
    }
    for (const json& hit : *hits) {
        if (results.size() >= static_cast<std::size_t>(std::max(1, max_results))) {
            break;
        }
        const std::string url = field(hit, "url");
        if (url.empty()) {
            continue;
        }
        results.push_back({field(hit, "title"), url, plain_text(field(hit, "content"))});
    }
    return results;
}

/// The results off DuckDuckGo's HTML page.
///
/// Each result is an anchor of class "result__a" with the page's address in
/// its href -- through a redirect of theirs, carrying the real address in a
/// "uddg" parameter -- and a sibling of class "result__snippet". Read with
/// string searches rather than an HTML parser, because there is none here
/// and the page's shape is three class names.
std::vector<SearchResult> parse_duckduckgo(std::string_view body, int max_results) {
    std::vector<SearchResult> results;
    std::size_t at = 0;
    while (results.size() < static_cast<std::size_t>(std::max(1, max_results))) {
        const std::size_t anchor = body.find("class=\"result__a\"", at);
        if (anchor == std::string_view::npos) {
            break;
        }
        const std::size_t open  = body.rfind("<a", anchor);
        const std::size_t close = body.find("</a>", anchor);
        const std::size_t tag_end = body.find('>', anchor);
        if (open == std::string_view::npos || close == std::string_view::npos
            || tag_end == std::string_view::npos || tag_end > close) {
            break;
        }
        // The whole opening tag: the href may sit before or after the class.
        const std::string_view tag  = body.substr(open, tag_end - open);
        const std::string_view href_at = [&]() -> std::string_view {
            const std::size_t h = tag.find("href=\"");
            if (h == std::string_view::npos) {
                return {};
            }
            const std::size_t end = tag.find('"', h + 6);
            return end == std::string_view::npos ? std::string_view{} : tag.substr(h + 6, end - h - 6);
        }();
        std::string url(href_at);
        // "//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com&rut=..." -- the
        // real address is the uddg parameter, percent-encoded.
        if (const std::size_t uddg = url.find("uddg="); uddg != std::string::npos) {
            std::string encoded = url.substr(uddg + 5);
            if (const std::size_t amp = encoded.find('&'); amp != std::string::npos) {
                encoded.resize(amp);
            }
            std::string decoded;
            for (std::size_t i = 0; i < encoded.size(); ++i) {
                if (encoded[i] == '%' && i + 2 < encoded.size()) {
                    decoded += static_cast<char>(std::stoi(encoded.substr(i + 1, 2), nullptr, 16));
                    i += 2;
                } else {
                    decoded += encoded[i] == '+' ? ' ' : encoded[i];
                }
            }
            url = decoded;
        } else if (url.rfind("//", 0) == 0) {
            url = "https:" + url;
        }
        const std::string title = plain_text(std::string(body.substr(tag_end + 1, close - tag_end - 1)));
        std::string snippet;
        const std::size_t snippet_at = body.find("result__snippet", close);
        if (snippet_at != std::string_view::npos) {
            const std::size_t next = body.find("class=\"result__a\"", close);
            if (next == std::string_view::npos || snippet_at < next) {
                const std::size_t s_open  = body.find('>', snippet_at);
                const std::size_t s_close = body.find("</a>", snippet_at);
                const std::size_t d_close = body.find("</div>", snippet_at);
                const std::size_t s_end   = std::min(s_close, d_close);
                if (s_open != std::string_view::npos && s_end != std::string_view::npos && s_open < s_end) {
                    snippet = plain_text(std::string(body.substr(s_open + 1, s_end - s_open - 1)));
                }
            }
        }
        at = close + 4;
        if (url.empty() || url.rfind("http", 0) != 0) {
            continue;
        }
        results.push_back({title.empty() ? url : title, url, snippet});
    }
    return results;
}

std::vector<SearchResult> parse_brave(const json& body, int max_results) {
    std::vector<SearchResult> results;
    const auto web = body.find("web");
    if (web == body.end() || !web->is_object()) {
        return results;
    }
    const auto hits = web->find("results");
    if (hits == web->end() || !hits->is_array()) {
        return results;
    }
    for (const json& hit : *hits) {
        if (results.size() >= static_cast<std::size_t>(std::max(1, max_results))) {
            break;
        }
        const std::string url = field(hit, "url");
        if (url.empty()) {
            continue;
        }
        results.push_back({field(hit, "title"), url, plain_text(field(hit, "description"))});
    }
    return results;
}

/// Trim the trailing slashes off a base URL so joining a path cannot double one.
std::string_view without_trailing_slash(std::string_view url) {
    while (!url.empty() && url.back() == '/') {
        url.remove_suffix(1);
    }
    return url;
}

/// A model's own tool call: the first JSON object carrying a string "query".
///
/// Scanned rather than parsed from a known position, because the call arrives
/// wrapped in whatever the model said around it -- "We need to search." then
/// the object, on the channel meant for tool calls.

std::string search_line_in(std::string_view reply) {
    constexpr std::string_view kMarker = "SEARCH:";
    std::size_t at = 0;
    while (at <= reply.size()) {
        const std::size_t end  = reply.find('\n', at);
        std::string_view  line = reply.substr(at, end == std::string_view::npos
                                                      ? std::string_view::npos
                                                      : end - at);
        // A line of its own, so that a model explaining how search works -- "you
        // can write SEARCH: followed by a query" -- is not taken as doing it.
        while (!line.empty() && (std::isspace(static_cast<unsigned char>(line.front())) != 0)) {
            line.remove_prefix(1);
        }
        while (!line.empty() && (std::isspace(static_cast<unsigned char>(line.back())) != 0)) {
            line.remove_suffix(1);
        }
        // Markdown emphasis around the marker is common enough to be worth
        // forgiving; the query itself is taken as written.
        while (!line.empty() && (line.front() == '*' || line.front() == '`')) {
            line.remove_prefix(1);
        }
        if (line.substr(0, kMarker.size()) == kMarker) {
            // Whatever is left, less the decoration a model wraps it in: the
            // emphasis it started with closes after the marker, and quoting the
            // query is common enough to be worth undoing.
            const auto decoration = [](char c) {
                return c == '*' || c == '`' || c == '"' ||
                       std::isspace(static_cast<unsigned char>(c)) != 0;
            };
            std::string_view query = line.substr(kMarker.size());
            while (!query.empty() && decoration(query.front())) {
                query.remove_prefix(1);
            }
            while (!query.empty() && decoration(query.back())) {
                query.remove_suffix(1);
            }
            return std::string(query);
        }
        if (end == std::string_view::npos) {
            break;
        }
        at = end + 1;
    }
    return {};
}


std::string tool_call_query(std::string_view text) {
    for (std::size_t at = text.find('{'); at != std::string_view::npos;
         at = text.find('{', at + 1)) {
        // Match braces so a nested object does not end the scan early.
        int depth = 0;
        for (std::size_t end = at; end < text.size(); ++end) {
            if (text[end] == '{') { ++depth; }
            if (text[end] == '}') {
                if (--depth != 0) {
                    continue;
                }
                const json parsed = json::parse(text.substr(at, end - at + 1), nullptr,
                                                /*allow_exceptions=*/false);
                if (!parsed.is_discarded()) {
                    if (const std::string query = field(parsed, "query"); !query.empty()) {
                        return query;
                    }
                }
                break;
            }
        }
    }
    return {};
}

}  // namespace

std::string request_url(const std::string& query, const SearchSettings& settings) {
    if (query.empty()) {
        return {};
    }
    const std::string encoded = percent_encode(query);
    const std::string limit   = std::to_string(std::clamp(settings.max_results, 1, 20));

    if (settings.provider == "duckduckgo") {
        return "https://html.duckduckgo.com/html/?q=" + encoded;
    }
    if (settings.provider == "wikipedia") {
        return "https://en.wikipedia.org/w/api.php?action=query&list=search&format=json"
               "&srlimit=" + limit + "&srsearch=" + encoded;
    }
    if (settings.provider == "searxng") {
        if (settings.endpoint.empty()) {
            return {};
        }
        return std::string(without_trailing_slash(settings.endpoint)) +
               "/search?format=json&q=" + encoded;
    }
    if (settings.provider == "brave") {
        return "https://api.search.brave.com/res/v1/web/search?count=" + limit + "&q=" + encoded;
    }
    return {};
}

std::vector<SearchResult> parse_results(std::string_view provider, std::string_view body,
                                        int max_results) {
    if (provider == "duckduckgo") {
        return parse_duckduckgo(body, max_results);
    }
    const json parsed = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return {};
    }
    if (provider == "wikipedia") { return parse_wikipedia(parsed, max_results); }
    if (provider == "searxng")   { return parse_searxng(parsed, max_results); }
    if (provider == "brave")     { return parse_brave(parsed, max_results); }
    return {};
}

std::vector<SearchResult> search(const std::string& query, const SearchSettings& settings,
                                 std::string& error) {
    error.clear();
    if (!settings.enabled) {
        error = "web search is off -- turn it on in Settings, under Tools";
        return {};
    }
    const std::string url = request_url(query, settings);
    if (url.empty()) {
        error = settings.provider == "searxng"
                    ? "the searxng provider needs the address of an instance in settings"
                    : "no search provider is configured";
        return {};
    }
    util::http::Request request;
    request.url             = url;
    request.timeout_seconds = std::clamp(settings.timeout_seconds, 1, 120);
    if (settings.provider == "duckduckgo") {
        // The HTML page is served to browsers; curl's own name gets a page
        // that asks to prove you are not a robot.
        request.headers = {{"User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                                          "(KHTML, like Gecko) Chrome/124.0 Safari/537.36"},
                           {"Accept", "text/html"}};
    }
    if (settings.provider == "brave") {
        if (settings.api_key.empty()) {
            error = "the brave provider needs an API key in settings";
            return {};
        }
        // In a header, and the header is in a file only this user can read.
        // It used to be an argument to curl, which put the key in the process
        // list for as long as the search took.
        request.headers = {{"X-Subscription-Token", settings.api_key},
                           {"Accept", "application/json"}};
    }

    const util::http::Response response = util::http::send(request);
    if (!response.ok()) {
        // Anything but a 2xx is a failure, not a body to parse. Handed to the
        // JSON reader, an error page comes back as an empty result -- "nothing
        // found" rather than "the key is wrong".
        error = "search failed (" + response.reason() + ")";
        return {};
    }
    const std::string& body = response.body;

    std::vector<SearchResult> results = parse_results(settings.provider, body,
                                                      settings.max_results);
    if (results.empty()) {
        error = "nothing found for \"" + query + "\"";
    }
    return results;
}

std::string format_for_model(const std::string& query,
                             const std::vector<SearchResult>& results) {
    if (results.empty()) {
        return "Web search for \"" + query + "\" returned nothing. Answer from what you know, "
               "and say that you could not look it up.";
    }
    std::string text = "Web search results for \"" + query + "\":\n\n";
    int index = 1;
    for (const SearchResult& result : results) {
        text += std::to_string(index++) + ". " + result.title + "\n";
        text += "   " + result.url + "\n";
        if (!result.snippet.empty()) {
            text += "   " + result.snippet + "\n";
        }
        text += "\n";
    }
    text += "Answer the original question using these where they help. Cite a source by its "
            "URL when you use it, and say so if they do not answer the question.";
    return text;
}

std::string search_request(std::string_view answer, std::string_view reasoning) {
    if (std::string asked = search_line_in(answer); !asked.empty()) {
        return asked;
    }
    // Nothing was said to the user, so whatever happened happened in the
    // thinking. See the header for why that is only read in this case.
    bool spoke = false;
    for (const char c : answer) {
        spoke = spoke || (std::isspace(static_cast<unsigned char>(c)) == 0);
    }
    if (spoke) {
        return {};
    }
    if (std::string asked = search_line_in(reasoning); !asked.empty()) {
        return asked;
    }
    return tool_call_query(reasoning);
}

std::string tool_instructions() {
    return "\n\nYou can look things up on the internet. If the question needs current "
           "information, or facts you are unsure of, reply with a single line and nothing "
           "else:\nSEARCH: <what to look up>\nThe results will be given to you and you then "
           "answer normally, citing what you used. Do not search for things you already know.";
}

}  // namespace crucible::tools
