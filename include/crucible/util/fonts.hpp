// SPDX-License-Identifier: MIT
//
// The fonts installed on this machine, by family.
//
// For the Font setting. A page cannot ask the system what is installed, so
// the list comes from here: fontconfig on Linux (the same place WebKitGTK
// finds its fonts), CoreText on a Mac, GDI on Windows. Each answers with the
// family names a stylesheet can ask for.
#pragma once

#include <string>
#include <vector>

namespace crucible::util {

/// Every font family installed, sorted, each once. Hidden system families (a
/// Mac's ".SF NS", Windows' vertical "@" faces) are left out. Empty when the
/// machine could not be asked, which the setting reads as "type a name".
std::vector<std::string> installed_font_families();

}  // namespace crucible::util
