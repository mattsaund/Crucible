// SPDX-License-Identifier: MIT
//
// Turning a block of text into the rows a code block draws.
//
// This is the half of a code block that is a decision rather than a drawing: is
// this a diff, which file is it about, what number does each line carry. None of
// it touches ImGui, which is the point -- the numbering of a unified diff is
// exactly the sort of thing that is wrong by one for a month before anybody
// looks closely, and it is only checkable if it can be called without a window.
//
// The drawing is in markdown_view.cpp and the colouring is in syntax.cpp.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace crucible::gui {

/// One drawn line: what to colour, and what to number it.
struct CodeRow {
    std::string text;         ///< the code, with any diff marker taken off
    char        marker = 0;   ///< 0 for plain code; ' ' '+' '-' '@' in a diff
    int         old_no = 0;   ///< line number on the left of a diff, 0 for none
    int         new_no = 0;   ///< and on the right
};

/// Columns, not bytes: a UTF-8 continuation byte occupies none of its own.
std::size_t columns(std::string_view text);

/// Is this a unified diff?
///
/// A hunk header settles it on its own -- nothing else in any language starts a
/// line with @@ and has another @@ after it, and both the diffs Crucible writes
/// itself ("@@ line 42 @@") and the ones a model pastes from git carry one.
///
/// Failing that, the block has to be mostly marked lines *and* carry both signs.
/// The "mostly marked" half alone is not enough, and the counter-example is
/// common: a YAML list, or a markdown bullet list inside a fence, is every line
/// starting with a minus. Requiring an addition as well as a removal costs only
/// the header-less pure-addition diff, which then renders as ordinary code -- a
/// much smaller wrong than colouring somebody's YAML as deletions.
bool looks_like_diff(std::string_view text);

/// The file a diff is about, from its `+++ b/path` line. Empty when it has none
/// -- a fragment of a diff with no header is still a diff worth colouring.
std::string diff_path(std::string_view text);

/// Split `text` into rows, working out the numbering as it goes.
///
/// `added` and `removed` come back with the counts the header shows. When
/// `diff` is false every line is itself and numbered from one, which is the
/// plain code-block case.
std::vector<CodeRow> code_rows(std::string_view text, bool diff, int& added,
                               int& removed);

}  // namespace crucible::gui
