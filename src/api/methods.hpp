// SPDX-License-Identifier: MIT
//
// What a method is, for the files that define them.
//
// Private to src/api. The surface's public face is one function that takes a
// string; this is the shape of what is behind it, shared between the handful
// of files the vocabulary is split across so that no one of them is the
// thousand-line chain of `if (method == ...)` this used to be.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "crucible/api/surface.hpp"

namespace crucible::api {

using json = nlohmann::json;

/// What a method says back: a result, or the reason there is none.
struct Reply {
    json        result;
    std::string error;   ///< empty when it worked
};

inline Reply good(json result = json::object()) {
    return Reply{std::move(result), {}};
}

inline Reply bad(std::string why) {
    // Never empty, because empty is how success is spelled.
    return Reply{nullptr, why.empty() ? std::string("it did not work") : std::move(why)};
}

/// What a lookup is allowed to know about the session: a copy, taken on the
/// session's thread before the lookup is sent somewhere else to run.
struct Scene {
    Config                config;
    std::filesystem::path project;   ///< empty when none is open
};

/// The two kinds of method. See Surface::prepare for why there are two.
///
/// A Lookup is handed a Scene and nothing else, so it can run on any thread.
/// An Action is handed the session itself, so it runs on the session's.
using Lookup = Reply (*)(const json& params, const Scene& scene);
using Action = Reply (*)(const json& params, Host& host);

struct Method {
    std::string_view name;
    Lookup           lookup = nullptr;
    Action           action = nullptr;
};

// --- the vocabulary, by subject ---------------------------------------------
//
// Each appends its methods to the table. Split by what the methods are about
// rather than by anything structural: the test for which file a new one goes
// in is which of these sentences it finishes.

void conversation_methods(std::vector<Method>& table);  ///< talking, cooking, history
void project_methods(std::vector<Method>& table);       ///< which folder, and may we
void roster_methods(std::vector<Method>& table);        ///< experts and where they are answered
void settings_methods(std::vector<Method>& table);      ///< the configuration and the machine
void install_methods(std::vector<Method>& table);       ///< runtimes and the trainer
void lab_methods(std::vector<Method>& table);           ///< making an expert

// --- shapes more than one file draws ----------------------------------------

/// The engine's state. In wire.cpp.
json snapshot_to_json(const Snapshot& snapshot);

/// One step of a cook's journal.
json cook_step_json(const CookStep& step);

/// What was attached to a turn or a cook, for the tiles above it.
json attachments_json(const std::vector<attach::Tile>& attachments);

/// The refusal every engine-moving method makes when there is no engine.
inline Reply no_engine() { return bad("the engine is not running"); }

}  // namespace crucible::api
