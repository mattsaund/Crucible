// SPDX-License-Identifier: MIT
// A routing decision: which expert answers, and how it was decided.
//
// The deciding is the orchestrator's, in Python -- see
// scripts/orchestrator/crucible_orchestrator/routing.py, and orchestra/link.hpp
// for how the core asks it. What is left here is the decision itself, which the
// engine acts on and the session history stores.
#pragma once

#include <string>
#include <string_view>

#include "crucible/routing/expert.hpp"

namespace crucible {

/// Where a routing decision came from, so the UI can be honest about how much
/// to trust it.
enum class RouteSource {
    Model,      ///< the router model chose it
    Keyword,    ///< keyword scoring chose it
    Fallback,   ///< nothing chose it; policy substituted this one
    Forced,     ///< the user pinned a subject with a slash command
};

struct RouteDecision {
    /// Empty, because that is what "nobody has decided yet" means. A
    /// default-constructed decision reaches the engine whenever the delegator
    /// could not run at all, and defaulting to a real expert sent every one of
    /// those prompts to that expert as though it had been chosen.
    ///
    /// An id rather than an index into the roster: a decision outlives the
    /// roster it was made against -- it is written into the session history and
    /// read back weeks later -- and an index would then name whichever expert
    /// had since moved into that slot.
    ExpertId    expert;
    float       confidence = 0.0F;
    RouteSource source     = RouteSource::Fallback;
    std::string detail;     ///< short human-readable note for the status line
};

std::string_view route_source_name(RouteSource source);

/// The inverse, for reading a stored session back. An unrecognized name is
/// Fallback, which is the honest answer for "this came from somewhere we no
/// longer understand".
RouteSource route_source_from_name(std::string_view name);

}  // namespace crucible
