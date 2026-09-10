// SPDX-License-Identifier: MIT
//
// Reading the markdown a model writes.
//
// Every instruction-tuned model answers in markdown whether or not you ask it
// to: headings, bold, bullets, fenced code. Printed as plain text that is a
// wall of asterisks and hashes, and the structure the model went to the trouble
// of expressing is left for the reader to reconstruct.
//
// This is the parsing half, kept away from the drawing half so it can be tested
// without a terminal. It is deliberately not a complete CommonMark
// implementation: it covers what models actually produce, and anything it does
// not recognize passes through as the text it was, which is exactly what used
// to happen to all of it.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace crucible::markdown {

/// A run of text with one style. The smallest thing the renderer draws.
struct Span {
    std::string text;
    bool code   = false;  ///< `like this`
    bool bold   = false;  ///< **like this**
    bool italic = false;  ///< *like this* or _like this_
};

/// What a line of markdown turns out to be.
enum class BlockKind {
    Paragraph,   ///< ordinary prose
    Heading,     ///< # .. ###### -- `level` says which
    Bullet,      ///< - * + -- `level` is the indent depth
    Numbered,    ///< 1. 2. 3. -- `marker` keeps the number the model wrote
    Quote,       ///< > quoted
    Code,        ///< inside a fence; `marker` is the language, if it said one
    Rule,        ///< --- or ***
    TableRow,    ///< one row of a table; the cells are in `cells`
    TableRule,   ///< the |---|:-:| under a table's header; `marker` holds the alignment
    Blank,       ///< a deliberate gap between blocks
};

/// One line of output, already classified and split into styled runs.
///
/// A line rather than a block, because the renderer draws line by line and a
/// paragraph's wrapping is the terminal's business, not this file's.
struct Block {
    BlockKind        kind  = BlockKind::Paragraph;
    int              level = 0;   ///< heading level, or list indent depth

    /// "1." for a numbered item, the language for code, and for a table's
    /// delimiter row one character of alignment per column: l, c or r.
    std::string      marker;

    std::vector<Span> spans;

    /// A table row's cells, each already split into styled runs. Empty for
    /// every other kind of block.
    std::vector<std::vector<Span>> cells;
};

/// Split `text` into blocks. Never fails: anything unrecognized is a paragraph.
std::vector<Block> parse(std::string_view text);

/// Split one line of inline markdown into styled runs. Exposed for testing;
/// `parse` calls it for every line that is not code.
std::vector<Span> parse_inline(std::string_view line);

}  // namespace crucible::markdown
