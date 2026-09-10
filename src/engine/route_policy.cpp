// SPDX-License-Identifier: MIT
#include "crucible/engine/route_policy.hpp"

#include <string>
#include <vector>

namespace crucible {
namespace {

/// The seat the user nominated to catch what does not fit, if it has a model.
///
/// Empty when none is set, or when the one that is set has since been ejected
/// or emptied -- a default expert that cannot answer is not a default expert,
/// and silently routing to it would produce the "no model configured" failure
/// one layer further down where it is harder to read.
ExpertId usable_default(const Config& config) {
    const ExpertId& chosen = config.routing.default_expert;
    return !chosen.empty() && config.has_expert(chosen) ? chosen : ExpertId{};
}

}  // namespace

RouteDecision apply_route_policy(const RouteDecision& proposed, const Config& config) {
    RouteDecision decision = proposed;
    const ExpertId backstop = usable_default(config);

    // The delegator answered, but not confidently. Treat that as "no decision"
    // and hand it to the default expert rather than committing to a specialist
    // on a coin flip. Pinned routes skip this: the user decided, not the model.
    //
    // With no default expert set there is nothing better to do than take the
    // answer, so the route stands and the detail records the doubt. That is a
    // change from the old behavior, which sent it to a built-in Fallback seat
    // that on most installs had no model either -- so the prompt failed instead
    // of being answered by the delegator's best guess.
    if (decision.source == RouteSource::Model && config.routing.min_confidence > 0.0F
        && decision.confidence < config.routing.min_confidence) {
        const std::string wanted = expert_label(config.roster, decision.expert);
        const std::string doubt =
            "undecided (" + wanted + " at "
            + std::to_string(static_cast<int>(decision.confidence * 100)) + "%)";
        if (!backstop.empty() && backstop != decision.expert) {
            decision.detail = doubt + "; used "
                            + expert_label(config.roster, backstop);
            decision.expert = backstop;
            decision.source = RouteSource::Fallback;
            return decision;
        }
        decision.detail = decision.detail.empty() ? doubt : doubt + "; " + decision.detail;
    }

    if (config.has_expert(decision.expert)) {
        return decision;
    }

    // The routed expert has no model behind it.
    const std::string wanted = decision.expert.empty()
        ? std::string("no expert")
        : expert_label(config.roster, decision.expert);

    if (!backstop.empty()) {
        decision.expert = backstop;
        decision.source = RouteSource::Fallback;
        decision.detail = wanted + " has no model; used "
                        + expert_label(config.roster, backstop);
        return decision;
    }

    // Nothing nominated either. Use any filled seat rather than failing, and say
    // plainly what happened -- an answer from the wrong specialist is worth more
    // than a refusal, as long as the transcript admits which one gave it.
    const std::vector<ExpertId> available = config.configured_experts();
    if (available.empty()) {
        decision.detail = "no experts have a model";
        return decision;
    }
    decision.expert = available.front();
    decision.source = RouteSource::Fallback;
    decision.detail = wanted + " has no model; used "
                    + expert_label(config.roster, decision.expert);
    return decision;
}

}  // namespace crucible
