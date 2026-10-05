// SPDX-License-Identifier: MIT
//
// Dropped files on Windows and macOS: the page reads them. See drops.hpp.
#include "drops.hpp"

namespace crucible::gui::drops {

bool watch(void* /*widget*/, std::function<void(std::vector<std::string>)> /*dropped*/) {
    return false;
}

}  // namespace crucible::gui::drops
