// SPDX-License-Identifier: MIT
//
// A model to build with, for a machine that has none.
//
// Crucible ships no models and no experts, and for somebody who knows what a
// GGUF is that is right: the seats are theirs from the first one. For
// somebody who installed Crucible to have a budgeting program made, it is a
// wall -- an empty roster routes nowhere, and a build with nobody to do the
// work fails before it plans. So on a machine with no expert seated, no
// provider added and nothing in the models folder, the first start fetches
// one: an open model that writes code, Apache-2.0, the largest of a short
// list that leaves the machine room to work, and seats it as Programming -- the expert every
// prompt falls back to and the model a build's new agents run on.
//
// Anything already there is left alone and nothing is fetched: a models
// folder with something in it, a seat with a model, or a provider. Those are
// somebody who has chosen, and the choice is theirs to make again in Settings.
#pragma once

#include <cstdint>
#include <string>

#include "crucible/config/config.hpp"

namespace crucible::starter {

/// One model on the list: where it is published, and what it costs.
struct Model {
    std::string   repo;    ///< "bartowski/Qwen2.5-Coder-7B-Instruct-GGUF"
    std::string   file;    ///< "Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf"
    std::string   label;   ///< "Qwen2.5 Coder 7B"
    std::uint64_t bytes = 0;

    /// Where it is downloaded from.
    std::string url() const;
};

/// The model for a machine with `memory` bytes of it: the largest whose
/// weights leave about half the machine free for the context, the system
/// and whatever else is open. 
Model for_memory(std::uint64_t memory);

/// Whether this configuration needs one fetched: no seat has a model, no
/// provider is added, and the models folder has nothing in it.
bool needed(const Config& config);

/// Seat `file` -- a model in the models folder -- as the one to build with:
/// a Programming seat when the roster has none, every seat with no model
/// given it, the build's new agents run on it, and prompts routed nowhere in
/// particular fall back to it.
void seat(Config& config, const std::string& file);

}  // namespace crucible::starter
