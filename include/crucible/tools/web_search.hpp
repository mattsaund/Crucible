// SPDX-License-Identifier: MIT
//
// Looking something up on the internet.
//
// The first thing Crucible does that leaves the machine, which is why it is off
// until it is switched on, why the query is shown in the transcript, and why
// this file is the only place that talks to a network.
//
// Four providers, reached by handing a URL to `curl`. Three return JSON, and
// JSON is preferred over scraping: a search engine's HTML is a moving target
// and its JSON is a contract, and the difference is whether this still works
// in six months. The fourth, DuckDuckGo, is the exception, read from its
// HTML results page, because it is the one real web index that asks for no
// key -- and an expert that cannot search until somebody signs up for one is
// an expert that does not search. `curl` rather than libcurl: Crucible deliberately builds
// without it -- the installer would otherwise need the development headers on
// every platform -- and a subprocess is the same way the runtime builder
// reaches git and cmake.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace crucible::tools {

/// One thing found.
struct SearchResult {
    std::string title;
    std::string url;
    std::string snippet;
};

/// Where to search and how.
struct SearchSettings {
    /// Off until switched on. Nothing here runs otherwise.
    bool enabled = false;

    /// duckduckgo | wikipedia | searxng | brave
    ///
    /// `duckduckgo` is the default: a real web index that needs neither a key
    /// nor a server, reached through its plain HTML results page -- which is
    /// the one provider here that is read by scraping rather than through a
    /// JSON contract, and so the one that may need its parser touched when the
    /// page changes. `wikipedia` is an encyclopedia, not a web index, and
    /// answers the kind of question an expert asks.
    ///
    /// `searxng` is a metasearch engine you run yourself -- point `endpoint` at
    /// it and this becomes web search without anyone learning what was asked.
    /// `brave` is a web index that wants an API key.
    std::string provider = "duckduckgo";

    /// Base URL for `searxng`, e.g. "http://localhost:8888". Unused otherwise.
    std::string endpoint;

    /// API key for `brave`. Unused otherwise.
    std::string api_key;

    int max_results     = 5;
    int timeout_seconds = 10;
};

/// The URL `query` becomes for these settings, or empty if the settings cannot
/// make one -- a searxng provider with no endpoint, say.
///
/// Exposed so the shape of the request can be tested without a network, and so
/// the transcript can show exactly what was fetched.
std::string request_url(const std::string& query, const SearchSettings& settings);

/// Read a provider's response body. Returns what it could find; a body that is
/// not JSON, or is JSON of an unexpected shape, yields nothing rather than
/// throwing.
std::vector<SearchResult> parse_results(std::string_view provider, std::string_view body,
                                        int max_results);

/// Run one search. Returns the results, or an empty list with `error` set.
///
/// Blocking, and bounded by `timeout_seconds`. Call it from the engine thread,
/// never from the one drawing the screen.
std::vector<SearchResult> search(const std::string& query, const SearchSettings& settings,
                                 std::string& error);

/// The results as the expert will read them: numbered, with the source of each,
/// and prefaced by what was asked. Empty results say so rather than producing a
/// heading with nothing under it.
std::string format_for_model(const std::string& query,
                             const std::vector<SearchResult>& results);

/// What the expert is asking to look up, or empty if it is not asking.
///
/// Two forms, because models fall into two camps.
///
/// The one Crucible asks for is a line of its own beginning `SEARCH:`. A text
/// protocol rather than the chat template's own tool-calling because Crucible
/// cannot know which model is in the seat: llama.cpp applies a template, it
/// does not negotiate a tool schema, and a convention every model can follow
/// beats one that only some can.
///
/// The other is what a model trained on tool use does regardless of what it was
/// asked: it writes a call in its own format, on a channel meant for tool
/// calls, which arrives here as reasoning. gpt-oss emits `{"query": "...",
/// "topn": 5}` and nothing at all on the channel the user reads. Ignoring that
/// would mean the tool never fires for the models most able to use it.
///
/// The reasoning is only consulted when `answer` is empty. A model that has
/// written something for the user is answering, not asking -- and a
/// programming expert whose reply contains a JSON object with a "query" field
/// is showing you code, not calling a tool.
std::string search_request(std::string_view answer, std::string_view reasoning);

/// What to add to an expert's system prompt so it knows the tool is there.
std::string tool_instructions();

}  // namespace crucible::tools
