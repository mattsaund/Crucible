// SPDX-License-Identifier: MIT
//
// Browsing Hugging Face from inside Crucible.
//
// The lab is only as approachable as the step where you choose what to start
// from. Sending someone to a browser to find a base model, copy a repository
// name and paste it back is where most of them stop, so the search lives in
// the tab: type what the model should know, see what there is, pick one.
//
// Read-only and unauthenticated: public repositories, no token, no account.
// Anything gated needs the browser, and the tab says so rather than failing
// with a 401 nobody can act on.
//
// The parsing is separated from the fetching on purpose -- the shapes the hub
// returns are what break, and a canned reply in a test is the only way to
// check them without a network.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace crucible::lab::hub {

/// What a search turned up.
struct Item {
    std::string id;        ///< "unsloth/Llama-3.2-1B"
    std::string author;    ///< the part before the slash
    std::string summary;   ///< the pipeline tag, or what the card is about

    std::uint64_t downloads = 0;
    std::uint64_t likes     = 0;

    /// Parameters in billions, when the hub says. Models carry it in their
    /// metadata; a name like "Llama-3.2-1B" is the fallback, and is why this
    /// can still be zero for a model that plainly has a size in its title.
    double parameters_b = 0.0;

    bool gated = false;
};

/// One file inside a repository.
struct File {
    std::string   path;
    std::uint64_t bytes = 0;
};

enum class Kind { Model, Dataset };

/// The URL a search asks for. Public, versioned, and documented by the hub.
std::string search_url(Kind kind, std::string_view query, int limit);

/// The URL that lists a repository's files.
std::string tree_url(Kind kind, std::string_view id);

/// The URL one file downloads from.
std::string download_url(Kind kind, std::string_view id, std::string_view file);

/// What a search replied, as items. Tolerant on purpose: a field the hub adds
/// or drops must not empty the list, because the list is the whole step.
std::vector<Item> parse_search(std::string_view json_text);

/// What a tree listing replied, as files.
std::vector<File> parse_tree(std::string_view json_text);

/// Parameters in billions read out of a repository name: "Llama-3.2-1B" is 1,
/// "Qwen2.5-7B-Instruct" is 7, "SmolLM-135M" is 0.135. Zero when the name says
/// nothing, which is not the same as a model with no parameters.
double parameters_from_name(std::string_view name);

/// Ask the hub. Returns false and sets `error` when curl is missing, the
/// network is not there, or the hub answered with something that is not JSON.
bool search(Kind kind, std::string_view query, int limit,
            std::vector<Item>& out, std::string& error);

/// List a repository's files.
bool tree(Kind kind, std::string_view id, std::vector<File>& out, std::string& error);

}  // namespace crucible::lab::hub
