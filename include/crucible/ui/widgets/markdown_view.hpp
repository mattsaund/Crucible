// SPDX-License-Identifier: MIT
//
// Drawing the markdown a model wrote. The parsing half is util/markdown.hpp,
// which knows nothing about terminals; this is the half that knows nothing
// about markdown beyond what that produced.
#pragma once

#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

namespace crucible::ui {

/// One element per line of `text`, styled.
///
/// Returned as a list rather than a single element so the caller can put it in
/// whatever it is already building -- and so a reply and the stats under it
/// stay in one vbox rather than becoming a box inside a box.
///
/// `dim_all` renders everything muted, for reasoning rather than an answer.
std::vector<ftxui::Element> render_markdown(const std::string& text, bool dim_all = false);

/// One file or snippet, drawn as a code block.
///
/// The same rendering a fenced block in a reply gets -- header, line numbers,
/// syntax colors, and the two-sided gutter of a diff -- for the places that
/// have code to show but no markdown around it. `language` may be a fence word
/// or a file name; empty lets it be guessed from the text.
ftxui::Element code_listing(const std::string& body, const std::string& language,
                            bool dim = false);

}  // namespace crucible::ui