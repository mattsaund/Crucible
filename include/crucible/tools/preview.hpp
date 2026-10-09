// SPDX-License-Identifier: MIT
//
// A page of the project, made showable.
//
// The window previews what is being built when what is being built is a web
// page: an HTML file in the project, drawn in a frame beside the code, with
// a dashboard for nudging its spacing, colors and type. The frame is given
// the page as a document rather than a location -- the window's own page is
// compiled in and has no origin, so a file in the project cannot be loaded
// from it by name -- which means the page has to arrive whole: its
// stylesheets, scripts and pictures put inside it. That is what this does,
// the way cmake/BundlePage.cmake does it for Crucible's own page.
//
// Only what is inside the project is inlined, resolved the way the workshop
// resolves a path. A reference to somewhere on the web is left as it is and
// loads or does not; a reference that escapes the project is left as a
// reference to nothing.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace crucible::tools::preview {

struct Bundle {
    bool        ok = false;
    std::string html;
    std::string error;
    std::vector<std::string> inlined;   ///< the files put inside, relative to the root
    std::vector<std::string> skipped;   ///< the ones that could not be: outside, missing, too large
};

/// `page` (relative to `root`) with its local stylesheets, scripts and
/// pictures inside it. `max_bytes` caps what any one file may add.
Bundle bundle(const std::filesystem::path& root, std::string_view page,
              std::size_t max_bytes = 6 * 1024 * 1024);

/// The HTML files in the project worth previewing, relative to the root,
/// index.html first and the rest in path order; folders nobody wants shown
/// (node_modules, build output, .git) are skipped.
std::vector<std::string> candidates(const std::filesystem::path& root, std::size_t limit = 40);

/// The MIME type for a file's extension, for a data: URI. Empty when it is
/// not something a page embeds.
std::string mime_of(const std::filesystem::path& path);

}  // namespace crucible::tools::preview
