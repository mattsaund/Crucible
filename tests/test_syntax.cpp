// SPDX-License-Identifier: MIT
//
// The code-block lexer.
//
// One property matters above every question of taste about which words are
// keywords: the pieces a line is split into must concatenate back to that line,
// byte for byte. The desktop renderer draws them end to end at a computed x,
// so a byte the lexer drops is a character missing from the middle of somebody's
// code, and a byte it duplicates is a character that appears twice. Neither
// looks like a lexer bug on screen -- it looks like the model wrote it wrong.
//
// The rest of the cases are the three things that are easy to get wrong and
// invisible until you hit them: a comment or a docstring that runs across
// lines, a quote that is really an apostrophe, and a language named by a file
// extension rather than by a fence.
#include "test_helpers.hpp"

#include "../src/gui/syntax.hpp"

namespace {

using crucible::gui::syntax::Carry;
using crucible::gui::syntax::Lang;
using crucible::gui::syntax::Piece;
using crucible::gui::syntax::Token;
using crucible::gui::syntax::highlight;
using crucible::gui::syntax::lang_from;

std::string joined(const std::vector<Piece>& pieces) {
    std::string out;
    for (const Piece& piece : pieces) {
        out += piece.text;
    }
    return out;
}

/// The token a given substring came out as, or Token::Text when it is not a
/// piece on its own.
Token token_of(const std::vector<Piece>& pieces, const std::string& text) {
    for (const Piece& piece : pieces) {
        if (piece.text == text) {
            return piece.token;
        }
    }
    return Token::Text;
}

bool holds(const std::vector<Piece>& pieces, const std::string& text, Token token) {
    for (const Piece& piece : pieces) {
        if (piece.text == text && piece.token == token) {
            return true;
        }
    }
    return false;
}

const std::vector<Lang>& all_languages() {
    static const std::vector<Lang> languages{
        Lang::None,  Lang::C,     Lang::Rust,  Lang::Go,       Lang::Java,
        Lang::JavaScript, Lang::Python, Lang::Shell, Lang::Ruby, Lang::Php,
        Lang::Lua,   Lang::Sql,   Lang::Json,  Lang::Yaml,     Lang::Toml,
        Lang::Ini,   Lang::Cmake, Lang::Html,  Lang::Css,      Lang::Markdown,
        Lang::Diff,
    };
    return languages;
}

/// Lines chosen to walk into every branch of the scanner: unterminated strings,
/// an apostrophe in prose, a lone bracket, tabs, a bare number, an empty line.
const std::vector<std::string>& awkward_lines() {
    static const std::vector<std::string> lines{
        "",
        " ",
        "\t\tindented",
        "#include <vector>",
        "/* opened and never closed",
        "*/ closed without being opened",
        "it's an apostrophe, not a string",
        "\"unterminated",
        "'''",
        "`backtick",
        "x = 3.14e-9;",
        "-- a comment",
        "}",
        "$HOME/${thing}",
        "a?b!c",
        "// \xE2\x86\x92 an arrow in a comment",
        "s = \"a \\\" b\" + 'c'",
        "@@ -1,2 +3,4 @@",
        "std::vector<int>::iterator it = v.begin();",
    };
    return lines;
}

}  // namespace

// ---------------------------------------------------------------------------

TEST(every_byte_of_every_line_comes_back_out) {
    // The invariant the renderer depends on. Checked across every language
    // against every awkward line, because the scanner is one loop shared by all
    // of them and a dropped byte would be a dropped byte everywhere.
    for (const Lang lang : all_languages()) {
        Carry carry;
        for (const std::string& line : awkward_lines()) {
            CHECK_EQ(joined(highlight(line, lang, carry)), line);
        }
    }
}

TEST(a_line_that_is_all_one_thing_is_one_piece) {
    // Runs of the same colour are merged as they are appended. Without that a
    // line of forty brackets is forty draw calls saying one thing, and the
    // renderer measures each of them.
    Carry carry;
    const std::vector<Piece> pieces = highlight("((((((((((", Lang::C, carry);
    CHECK_EQ(pieces.size(), std::size_t{1});
    CHECK(pieces.front().token == Token::Punct);
}

TEST(a_block_comment_stays_open_across_lines) {
    Carry carry;
    std::vector<Piece> pieces = highlight("/* the comment opens", Lang::C, carry);
    CHECK(carry.in_comment);
    CHECK(holds(pieces, "/* the comment opens", Token::Comment));

    // The middle of it is a comment even though nothing on the line says so.
    pieces = highlight("   int not_really_code = 1;", Lang::C, carry);
    CHECK(carry.in_comment);
    CHECK_EQ(pieces.size(), std::size_t{1});
    CHECK(pieces.front().token == Token::Comment);

    // And it closes, with the code after it coloured again.
    pieces = highlight("*/ int x = 1;", Lang::C, carry);
    CHECK(!carry.in_comment);
    CHECK(holds(pieces, "int", Token::Type));
    CHECK(holds(pieces, "1", Token::Number));
}

TEST(a_docstring_stays_open_across_lines) {
    Carry carry;
    std::vector<Piece> pieces = highlight("    \"\"\"What it does.", Lang::Python, carry);
    CHECK(carry.in_string);

    pieces = highlight("    Still the docstring.", Lang::Python, carry);
    CHECK(carry.in_string);
    CHECK(holds(pieces, "    Still the docstring.", Token::String));

    pieces = highlight("    \"\"\"", Lang::Python, carry);
    CHECK(!carry.in_string);

    // Code after it is code again.
    pieces = highlight("    return 1", Lang::Python, carry);
    CHECK(holds(pieces, "return", Token::Keyword));
}

TEST(an_apostrophe_does_not_open_a_string_that_never_closes) {
    // A single quote that never closes is far more often an apostrophe in a
    // comment or a sentence than a string running onto the next line. Carrying
    // it would paint the whole rest of the block green.
    Carry carry;
    highlight("it's a note, not a string", Lang::Python, carry);
    CHECK(!carry.in_string);
    CHECK(carry.quote.empty());
}

TEST(the_language_can_be_named_by_a_fence_or_by_a_file) {
    // A fence says "```python" and a diff says "+++ b/main.py". Both answer the
    // same question, so both go through the same door.
    CHECK(lang_from("python") == Lang::Python);
    CHECK(lang_from("main.py") == Lang::Python);
    CHECK(lang_from("src/gui/syntax.cpp") == Lang::C);
    CHECK(lang_from("cpp") == Lang::C);
    CHECK(lang_from("Cargo.toml") == Lang::Toml);
    CHECK(lang_from("diff") == Lang::Diff);
    CHECK(lang_from("patch") == Lang::Diff);
    CHECK(lang_from("CMakeLists.txt") == Lang::Cmake);

    // A fence sometimes carries more than the language.
    CHECK(lang_from("python title=\"x\"") == Lang::Python);

    // And something it has never heard of is not an error, it is plain text.
    CHECK(lang_from("brainfuck") == Lang::None);
    CHECK(lang_from("") == Lang::None);
}

TEST(the_obvious_things_get_the_obvious_colours) {
    Carry carry;
    const std::vector<Piece> c = highlight("static int count = 42;  // how many",
                                           Lang::C, carry);
    CHECK(holds(c, "static", Token::Keyword));
    CHECK(holds(c, "int", Token::Type));
    CHECK(holds(c, "42", Token::Number));
    CHECK(holds(c, "// how many", Token::Comment));

    Carry py;
    const std::vector<Piece> p = highlight("def render(self, text):", Lang::Python, py);
    CHECK(holds(p, "def", Token::Keyword));
    // An identifier with a bracket touching it is being called, which is the
    // one thing here that cannot be looked up in a table.
    CHECK(holds(p, "render", Token::Function));
    CHECK(holds(p, "self", Token::Type));

    // ...and `if (x)` must not turn `if` into a function in every C-like
    // language on earth.
    Carry cc;
    const std::vector<Piece> k = highlight("if (x) return;", Lang::C, cc);
    CHECK(token_of(k, "if") == Token::Keyword);
}

TEST(a_language_with_no_rules_leaves_the_line_alone) {
    // Lang::None is what an unknown fence and a plain command's output get, and
    // it has to be one untouched run -- a shell transcript coloured as though
    // it were C is worse than no colour at all.
    Carry carry;
    const std::vector<Piece> pieces =
        highlight("error: expected ';' before '}' token", Lang::None, carry);
    CHECK_EQ(pieces.size(), std::size_t{1});
    CHECK(pieces.front().token == Token::Text);
}
