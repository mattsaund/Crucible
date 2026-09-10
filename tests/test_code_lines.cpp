// SPDX-License-Identifier: MIT
//
// Reading a code block before drawing it.
//
// Three questions, and all three are wrong in ways nobody notices for a month.
//
// Is it a diff? Say yes about a YAML list and every line of somebody's config
// turns red. Say no about a real diff and the additions and deletions vanish
// into gray.
//
// What number does each line carry? A unified diff numbers two files at once,
// and off-by-one here is a line number that points at the line above the one it
// labels -- which is worse than no number, because it is believed.
//
// Which file is it about? The header a model pastes says `+++ b/path`, and the
// `b/` is git's bookkeeping rather than part of the path.
#include "test_helpers.hpp"

#include "crucible/util/code_lines.hpp"

namespace {

using crucible::syntax::CodeRow;
using crucible::syntax::code_rows;
using crucible::syntax::columns;
using crucible::syntax::diff_path;
using crucible::syntax::looks_like_diff;

/// The rows of a diff, with the counts thrown away.
std::vector<CodeRow> rows_of(const std::string& text, bool diff = true) {
    int added   = 0;
    int removed = 0;
    return code_rows(text, diff, added, removed);
}

/// A diff in the shape git writes, and in the shape util/diff.cpp writes.
const char* kGitDiff =
    "--- a/tictactoe.py\n"
    "+++ b/tictactoe.py\n"
    "@@ -30,4 +30,5 @@ def is_full(board):\n"
    "     return all(cell != ' ' for cell in board)\n"
    "-def computer_move(board):\n"
    "-    return random.choice(free)\n"
    "+def computer_move(board):\n"
    "+    _, move = minimax(board, 'O', 'O')\n"
    "+    return move\n";

/// What util/diff.cpp emits: two lines of context, then the changes, and a
/// header naming the first *changed* line rather than the first line shown.
const char* kCrucibleDiff =
    "@@ line 42 @@\n"
    " context one\n"
    " context two\n"
    "-was this\n"
    "+is this now\n"
    " context three\n";

}  // namespace

// ---------------------------------------------------------------------------

TEST(a_diff_is_recognized_without_being_told) {
    CHECK(looks_like_diff(kGitDiff));
    CHECK(looks_like_diff(kCrucibleDiff));

    // A hunk header on its own is enough, even with nothing marked under it.
    CHECK(looks_like_diff("@@ -1,2 +1,2 @@\n unchanged\n"));
}

TEST(a_list_of_bullets_is_not_a_diff) {
    // The counter-example that made the old "half the lines are marked" rule
    // wrong: every line of a YAML list starts with a minus, and coloring one
    // as a page of deletions is a much bigger wrong than missing a diff.
    CHECK(!looks_like_diff("- one\n- two\n- three\n"));
    CHECK(!looks_like_diff("steps:\n  - build\n  - test\n  - install\n"));

    // A markdown list inside a fence, same shape.
    CHECK(!looks_like_diff("- first point\n- second point\n"));

    // And ordinary code, which has neither markers nor a header.
    CHECK(!looks_like_diff("int main() {\n    return 0;\n}\n"));

    // One line is never enough to decide on counts alone.
    CHECK(!looks_like_diff("-x"));
}

TEST(a_git_hunk_numbers_both_sides_from_its_header) {
    const std::vector<CodeRow> rows = rows_of(kGitDiff);

    // The preamble carries no numbers: it is not a line of the file.
    CHECK(rows[0].marker == '@');
    CHECK_EQ(rows[0].old_no, 0);
    CHECK_EQ(rows[1].old_no, 0);
    CHECK(rows[2].marker == '@');

    // The context row starts both sides at what the header said.
    CHECK(rows[3].marker == ' ');
    CHECK_EQ(rows[3].old_no, 30);
    CHECK_EQ(rows[3].new_no, 30);

    // A removed line advances the old side only...
    CHECK(rows[4].marker == '-');
    CHECK_EQ(rows[4].old_no, 31);
    CHECK_EQ(rows[4].new_no, 0);
    CHECK_EQ(rows[5].old_no, 32);

    // ...and an added line advances the new side only.
    CHECK(rows[6].marker == '+');
    CHECK_EQ(rows[6].old_no, 0);
    CHECK_EQ(rows[6].new_no, 31);
    CHECK_EQ(rows[7].new_no, 32);
    CHECK_EQ(rows[8].new_no, 33);
}

TEST(crucible_own_hunk_header_counts_its_context_back_off) {
    // "@@ line 42 @@" names the first changed line. Two context rows are drawn
    // above it, so the first row on screen is line 40 -- and the count comes
    // from looking at the rows rather than from knowing what util/diff.cpp's
    // context setting happens to be today.
    const std::vector<CodeRow> rows = rows_of(kCrucibleDiff);

    CHECK(rows[0].marker == '@');
    CHECK_EQ(rows[1].old_no, 40);
    CHECK_EQ(rows[2].old_no, 41);
    CHECK(rows[3].marker == '-');
    CHECK_EQ(rows[3].old_no, 42);      // the line the header named
    CHECK(rows[4].marker == '+');
    CHECK_EQ(rows[4].new_no, 42);
    CHECK_EQ(rows[5].old_no, 43);
}

TEST(the_marker_column_is_taken_off_the_text) {
    // What gets drawn is the code, not the code with a plus in front of it --
    // otherwise every added line in a diff is indented one column further than
    // the context around it, and the alignment a diff is read by is gone.
    const std::vector<CodeRow> rows = rows_of(kCrucibleDiff);
    CHECK_EQ(rows[1].text, std::string("context one"));
    CHECK_EQ(rows[3].text, std::string("was this"));
    CHECK_EQ(rows[4].text, std::string("is this now"));
}

TEST(the_counts_are_what_the_header_shows) {
    int added   = 0;
    int removed = 0;
    code_rows(kGitDiff, true, added, removed);
    CHECK_EQ(added, 3);
    CHECK_EQ(removed, 2);
}

TEST(plain_code_is_numbered_from_one_and_left_alone) {
    int added   = 0;
    int removed = 0;
    const std::vector<CodeRow> rows =
        code_rows("first\n-second\n+third\n", false, added, removed);
    CHECK_EQ(rows.size(), std::size_t{3});
    CHECK_EQ(rows[0].new_no, 1);
    CHECK_EQ(rows[1].new_no, 2);
    CHECK_EQ(rows[2].new_no, 3);
    // Not a diff, so the markers are part of the code and stay in it.
    CHECK_EQ(rows[1].text, std::string("-second"));
    CHECK_EQ(added, 0);
    CHECK_EQ(removed, 0);
}

TEST(a_trailing_newline_is_not_an_extra_line) {
    int added   = 0;
    int removed = 0;
    const std::vector<CodeRow> with    = code_rows("one\ntwo\n", false, added, removed);
    const std::vector<CodeRow> without = code_rows("one\ntwo",   false, added, removed);
    CHECK_EQ(with.size(), std::size_t{2});
    CHECK_EQ(without.size(), std::size_t{2});
}

TEST(the_file_a_diff_is_about_comes_off_its_header) {
    CHECK_EQ(diff_path(kGitDiff), std::string("tictactoe.py"));

    // A tab and a timestamp is what plain `diff -u` writes.
    CHECK_EQ(diff_path("--- a/x.c\n+++ b/x.c\t2026-09-06 12:00:00\n"),
             std::string("x.c"));

    // A file being deleted has no new path worth naming.
    CHECK_EQ(diff_path("--- a/gone.txt\n+++ /dev/null\n"), std::string());

    // And a fragment with no header is still a diff; it just cannot say which.
    CHECK_EQ(diff_path(kCrucibleDiff), std::string());
}

TEST(a_column_is_a_glyph_and_not_a_byte) {
    // The renderer places every piece at a computed x, which is the column
    // count times one measurement. Counting bytes would push a line with an
    // arrow or an em dash in it two columns to the right of where it belongs.
    CHECK_EQ(columns("plain"), std::size_t{5});
    CHECK_EQ(columns("a \xE2\x86\x92 b"), std::size_t{5});      // a -> b
    CHECK_EQ(columns("\xE2\xA0\x80\xE2\xA0\x80"), std::size_t{2});  // two braille cells
    CHECK_EQ(columns(""), std::size_t{0});
}
