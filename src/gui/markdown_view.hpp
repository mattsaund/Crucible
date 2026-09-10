// SPDX-License-Identifier: MIT
//
// Drawing the markdown a model wrote.
//
// The parsing is shared with the terminal (util/markdown.hpp), so both faces
// break a reply into the same blocks and only the drawing differs. This is the
// drawing.
//
// ImGui has no rich text: a run of styled words is not something you can hand
// to TextWrapped, because the font changes mid-line and the wrapping has to
// know about it. So paragraphs are laid out here, word by word, against the
// available width -- which is also what makes an inline `code` span able to
// carry a background without the rest of the line inheriting it.
#pragma once

#include <string>
#include <string_view>

#include <imgui.h>

namespace crucible::gui {

/// Render `text` as markdown at the current cursor.
///
/// `base` is the color ordinary prose takes; headings, code and quotes have
/// their own. Consumes the full available width and advances the cursor past
/// what it drew.
void draw_markdown(std::string_view text, ImU32 base);

/// Render `text` as one fenced code block, without parsing it as markdown.
///
/// For content that is code by construction and must not be reinterpreted: a
/// diff, or a command's captured output. A `#` at the start of a shell line is
/// a comment, and passing it through the markdown parser would make it a
/// heading.
///
/// The block draws its own header (the language, the file it belongs to, how
/// long it is, a button that copies it), numbers every line down the left, and
/// colors the code with gui/syntax.hpp. Unified diffs are recognized without
/// being told: the marker in column one turns into a gutter of old and new line
/// numbers and a washed row, and the code on the row is still syntax-colored,
/// because an added line you cannot read is not much of an improvement on a
/// removed one.
///
/// `language` is a fence marker ("python") or a file name ("main.py") -- both
/// answer the same question. `caption` names the file the block is about, when
/// the caller knows and the text does not say. `seq` distinguishes one block
/// from the next within whatever ID the caller has pushed, so that expanding a
/// long block stays expanded on the next frame.
void draw_code_block(std::string_view text, std::string_view language = {},
                     std::string_view caption = {}, int seq = 0);

}  // namespace crucible::gui
