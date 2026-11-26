// SPDX-License-Identifier: MIT
//
// How hard a model thinks, and whether it thinks at all: what each one takes.
//
// Every reasoning model has a dial, and no two are marked the same. gpt-oss
// reads low, medium or high from its system message. Qwen 3.8's template takes
// low, medium or xhigh and refuses anything else outright. Inkling 2 Mini's
// maps seven words to a number. Claude takes low to max, and its Models API
// says which of them each model has; OpenAI's take none to max, model by
// model; Gemini's door takes four. Thinking itself can be switched off on
// some, is always on for others, and is not there at all on most models
// people run at home.
//
// A Profile is what one model takes, so the box can offer exactly that: the
// levels it has, and a thinking switch only where there is one to switch. A
// choice is kept as a word from the one scale below, and nearest() turns it
// into what a model takes where the two differ.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "crucible/config/config.hpp"

namespace crucible::reasoning {

/// Every effort level there is, from least to most: minimal, low, medium,
/// high, xhigh, max. "none" is not on it: that is thinking switched off,
/// which is a switch of its own.
const std::vector<std::string>& scale();

/// Where `level` is on the scale, or -1 when it is not on it.
int rank(std::string_view level);

/// Whether a model thinks before it answers, and whether that can change.
enum class Thinking {
    None,      ///< it does not think: there is nothing to switch
    Always,    ///< it always does, and cannot be told not to
    Optional,  ///< it can be told to think or not
};

std::string_view thinking_word(Thinking thinking);

/// What one model takes.
struct Profile {
    /// The effort levels it takes, from the scale and in its order. Empty
    /// when it takes none.
    std::vector<std::string> levels;

    /// The level it uses when none is asked for, when that is known.
    std::string standard;

    Thinking thinking = Thinking::None;

    /// For Optional: whether it thinks when nothing is said either way.
    bool thinks_unasked = true;

    /// Thinking is switched off by asking for the effort "none" rather than
    /// by a switch of its own: Inkling 2 Mini's template, OpenAI's models,
    /// Gemini's door.
    bool off_is_none = false;

    /// Where this was learned: "template", "provider" (asked, as Anthropic's
    /// Models API answers), "name" (a model known by its name), or empty when
    /// nothing could be learned.
    std::string source;

    bool takes_effort() const { return !levels.empty(); }
    bool can_switch() const { return thinking == Thinking::Optional; }
};

/// The level `wanted` comes to on a model that takes `levels`: itself when
/// it is one of them, and otherwise the nearest of them on the scale, the
/// higher of two as near. Empty when nothing was asked for, or the model
/// takes no effort.
std::string nearest(const std::vector<std::string>& levels, std::string_view wanted);

/// What a chat template takes, found by rendering it: with each level of the
/// scale as `reasoning_effort` (a level it refuses, or writes the same as a
/// higher one, is not its), and with thinking switched off. Empty for a
/// template that cannot be read. See reasoning.cpp for the rules.
Profile from_template(const std::string& source);

/// What Anthropic's Models API says about a model: the levels its
/// `capabilities.effort` marks as supported, and whether its thinking can be
/// switched off (`capabilities.thinking.types`).
Profile from_anthropic(const nlohmann::json& capabilities);

/// What a model at a provider that speaks OpenAI's shape is known to take,
/// by its name and the address it is asked at. A table, and so out of date
/// the week it is written: a provider's refusal teaches the request what it
/// does take (see remote_model.hpp, Quirks).
Profile from_name(std::string_view endpoint, std::string_view model);

/// The same for a Claude asked through a gateway that has no Models API.
Profile claude_by_name(std::string_view model);

/// What the box asked for of a seat: its own effort and thinking, or the
/// box's when it has none ("" for each means the model's own way).
struct Asked {
    std::string effort;     ///< a word on the scale, or empty
    std::string thinking;   ///< "on", "off", or empty
};

/// The seat's own setting where it has one, the box's where it does not.
Asked asked_of(const Config& config, const ModelParams& seat);

/// A profile as the page draws it.
nlohmann::json to_json(const Profile& profile);

}  // namespace crucible::reasoning
