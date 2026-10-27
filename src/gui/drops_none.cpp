// SPDX-License-Identifier: MIT
//
// Dropped and pasted files on Windows and macOS: the page reads them. See
// drops.hpp.
#include "drops.hpp"

namespace crucible::gui::drops {

bool watch(void* /*widget*/, std::function<void(std::vector<std::string>)> /*dropped*/) {
    return false;
}

bool watch_clipboard(void* /*widget*/, std::function<void(std::vector<std::string>)> /*copied*/) {
    return false;
}

void read_clipboard(void* /*widget*/) {}

}  // namespace crucible::gui::drops
