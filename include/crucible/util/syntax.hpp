// SPDX-License-Identifier: MIT
//
// Coloring the code a model wrote.
//
// This is a lexer, not a parser. It has no idea what a program means and is not
// trying to find out: it splits a line into runs of "this is a string", "this
// is a keyword", "this is a comment", and gets those right often enough that a
// forty-line function reads as structure instead of as a gray block. A real
// grammar per language would be a thousand lines each and would still be wrong
// on the half-written snippets models actually emit.
//
// It works a line at a time, because that is how the renderer draws and how a
// diff arrives -- but a block comment and a triple-quoted string both outlive
// their line, so the caller carries a `Carry` from one line to the next.
//
// Anything it does not recognize comes out as Text, which is exactly what the
// whole block used to be.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace crucible::syntax {

/// The languages worth telling apart.
///
/// Not a list of every language: a list of the ones whose *lexical* rules
/// differ. Java, C# and Go all take C's comments and quotes and differ only in
/// their keyword sets, so they are one entry with three word lists behind it.
enum class Lang {
    None,
    C,          ///< and C++, ObjC -- /* */, //, ' and "
    Rust,
    Go,
    Java,       ///< and C#, Kotlin, Swift, Scala
    JavaScript, ///< and TypeScript -- adds the backtick
    Python,     ///< # comments, ''' and """
    Shell,      ///< # comments, $variables
    Ruby,
    Php,
    Lua,
    Sql,
    Json,
    Yaml,
    Toml,
    Ini,
    Cmake,
    Html,       ///< and XML
    Css,
    Markdown,
    Diff,
};

/// The language a fence marker or a file name names.
///
/// Takes either: "```python" gives Python and so does "main.py", because a diff
/// says which file it is patching and a fence says which language it is, and
/// both answer the same question.
Lang lang_from(std::string_view marker);

/// What to call it in a code block's header. Empty for Lang::None.
std::string_view lang_name(Lang lang);

/// Guess the language from the code itself.
///
/// For the fence that says nothing. Models open a block with a bare ``` about
/// as often as they name the language, and a block that is not colored because
/// nobody said what it was looks like a block the program failed on -- the
/// reader cannot tell "we did not know" from "we got it wrong".
///
/// Scored on marks that are close to unique to one language: a shebang, an
/// `#include`, `fn main(`, `<?php`. A tie or a weak best guess returns None,
/// which is the honest answer for a paragraph of English in a code fence, and
/// leaves it drawn as plain text exactly as it is now.
Lang sniff(std::string_view body);

/// What one run of characters turned out to be.
enum class Token {
    Text,
    Keyword,   ///< if, return, func, def
    Type,      ///< int, String, a capitalised name where that means a type
    String,    ///< including the quotes, and char literals
    Number,
    Comment,
    Function,  ///< an identifier with a ( immediately after it
    Punct,     ///< brackets, operators, semicolons
    Preproc,   ///< #include, a shell $variable, an attribute
};

/// One run of characters that share a color.
struct Piece {
    std::string text;
    Token       token = Token::Text;
};

/// What a line leaves behind for the next one.
///
/// A block comment or a triple-quoted string opened on line 12 is still open on
/// line 13, and a lexer that forgets that colors the body of every docstring
/// as code. `quote` holds which delimiter is still waiting to be closed.
struct Carry {
    bool        in_comment = false;  ///< inside /* */ or <!-- --> or =begin
    bool        in_string  = false;  ///< inside a multi-line string
    std::string quote;               ///< the delimiter that will close it
};

/// Split one line into colored runs, advancing `carry`.
///
/// The pieces concatenate back to `line` exactly -- every byte comes out
/// somewhere, so the renderer can lay them end to end and get the original
/// line, spacing and all.
std::vector<Piece> highlight(std::string_view line, Lang lang, Carry& carry);

}  // namespace crucible::syntax
