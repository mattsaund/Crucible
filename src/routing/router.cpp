// SPDX-License-Identifier: MIT
//
// See router.hpp. The deciding is the orchestrator's now, in Python; what is
// left is the names a decision's source is stored under in a session, which
// sessions written before the orchestrator existed share.
#include "crucible/routing/router.hpp"

namespace crucible {

std::string_view route_source_name(RouteSource source) {
    switch (source) {
        case RouteSource::Model:    return "router model";
        case RouteSource::Keyword:  return "keywords";
        case RouteSource::Forced:   return "pinned";
        case RouteSource::Fallback: break;
    }
    return "fallback";
}

RouteSource route_source_from_name(std::string_view name) {
    if (name == "router model") { return RouteSource::Model; }
    if (name == "keywords")     { return RouteSource::Keyword; }
    if (name == "pinned")       { return RouteSource::Forced; }
    return RouteSource::Fallback;
}

}  // namespace crucible
