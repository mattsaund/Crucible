// SPDX-License-Identifier: MIT
//
// See markdown_view.hpp.
#include "crucible/ui/widgets/markdown_view.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "crucible/util/code_lines.hpp"
#include "crucible/util/syntax.hpp"

#include "crucible/ui/theme.hpp"
#include <ftxui/dom/table.hpp>

#include "crucible/util/markdown.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace)

namespace crucible::ui {
namespace {

// ---------------------------------------------------------------------------
// Code blocks
// ---------------------------------------------------------------------------
//
// The same block the window draws, in the parts a terminal has: a header saying
// what it is, a gutter of line numbers, syntax colors, and the two-sided
// numbering of a diff. Everything below shares util/syntax and util/code_lines
// with the desktop app, so the two faces cannot disagree about what a keyword
// is or which line a hunk starts on -- only about how it is painted.
//
// No fold. The window folds a long block because the pointer is there to open
// it again; a terminal scrolls, which is the same answer arrived at for free.

ftxui::Color color_of(syntax::Token token) {
    switch (token) {
        case syntax::Token::Keyword:  return theme::kCodeKeyword;
        case syntax::Token::Type:     return theme::kCodeType;
        case syntax::Token::String:   return theme::kCodeString;
        case syntax::Token::Number:   return theme::kCodeNumber;
        case syntax::Token::Comment:  return theme::kCodeComment;
        case syntax::Token::Function: return theme::kCodeFunction;
        case syntax::Token::Punct:    return theme::kCodePunct;
        case syntax::Token::Preproc:  return theme::kCodePreproc;
        case syntax::Token::Text:     break;
    }
    return theme::kCodeText;
}

/// A number right-aligned in `width` columns, or that many spaces for a row
/// that has no number on this side of a diff.
std::string gutter_number(int value, int width) {
    const std::string digits = value > 0 ? std::to_string(value) : std::string();
    return std::string(static_cast<std::size_t>(width) - digits.size(), ' ') + digits;
}

/// One block of code, gathered from a run of Code lines.
Element code_block_impl(const std::string& body, const std::string& fence, bool dim_all) {
    syntax::Lang lang = syntax::lang_from(fence);
    const bool   diff = lang == syntax::Lang::Diff || syntax::looks_like_diff(body);
    std::string  path;
    if (diff) {
        path = syntax::diff_path(body);
        lang = syntax::lang_from(path);
    }
    if (lang == syntax::Lang::None) {
        // Nothing said what this is, so look at it. A fence opened with a bare
        // ``` is as common as one that names its language.
        lang = syntax::sniff(body);
    }

    int added   = 0;
    int removed = 0;
    const std::vector<syntax::CodeRow> rows = syntax::code_rows(body, diff, added, removed);

    int widest_old = 0;
    int widest_new = 0;
    for (const syntax::CodeRow& row : rows) {
        widest_old = std::max(widest_old, row.old_no);
        widest_new = std::max(widest_new, row.new_no);
    }
    const auto digits = [](int value) {
        return static_cast<int>(std::to_string(std::max(value, 1)).size());
    };
    const int old_width = diff ? digits(widest_old) : 0;
    const int new_width = digits(widest_new);

    Elements lines;

    // The header: what it is, which file, and how much moved.
    {
        std::string left(diff ? "diff" : syntax::lang_name(lang));
        if (left.empty()) {
            left = "text";
        }
        if (!path.empty()) {
            left += "  ·  " + path;
        }
        const std::string right =
            diff ? "+" + std::to_string(added) + " -" + std::to_string(removed)
                 : std::to_string(rows.size())
                       + (rows.size() == 1 ? " line" : " lines");
        lines.push_back(hbox({
            text("  " + left) | color(dim_all ? theme::kMeta : theme::kMeta),
            filler(),
            text(right + "  ") | color(theme::kMeta),
        }));
    }

    syntax::Carry carry;
    for (const syntax::CodeRow& row : rows) {
        // Lexed whether or not it is drawn dim: a block comment opened on one
        // line still has to be closed on the next.
        const std::vector<syntax::Piece> pieces = syntax::highlight(row.text, lang, carry);

        Elements parts;
        parts.push_back(text("  "));
        if (diff) {
            parts.push_back(text(gutter_number(row.old_no, old_width) + " ")
                            | color(theme::kCodeGutter));
        }
        parts.push_back(text(gutter_number(row.new_no, new_width) + " ")
                        | color(theme::kCodeGutter));

        const bool plus  = row.marker == '+';
        const bool minus = row.marker == '-';
        if (diff) {
            parts.push_back(text(std::string(1, plus ? '+' : minus ? '-' : ' ') + " ")
                            | color(plus  ? theme::kDiffAdded
                                    : minus ? theme::kDiffRemoved
                                            : theme::kCodeGutter));
        }

        if (row.marker == '@') {
            // A hunk or file header: the structure of the diff, in the color
            // structure takes everywhere else.
            parts.push_back(text(row.text) | color(theme::kHeading));
        } else {
            for (const syntax::Piece& piece : pieces) {
                parts.push_back(text(piece.text)
                                | color(dim_all ? theme::kMeta : color_of(piece.token)));
            }
        }

        Element line = hbox(std::move(parts));
        if (plus) {
            line = line | bgcolor(theme::kDiffAddedBg);
        } else if (minus) {
            line = line | bgcolor(theme::kDiffRemovedBg);
        }
        lines.push_back(std::move(line));
    }
    return vbox(std::move(lines));
}

/// One styled run.
Element span_element(const markdown::Span& span, bool dim_all) {
    Element piece = text(span.text);
    if (span.code) {
        piece = piece | color(theme::kCode);
    }
    if (span.bold) {
        piece = piece | bold;
    }
    if (span.italic) {
        piece = piece | italic;
    }
    return dim_all ? piece | color(theme::kMeta) | dim : piece;
}

/// A line's runs, wrapped at the panel edge.
///
/// `paragraph` cannot be used: it takes one string and does its own splitting,
/// so it would flatten the styling. This is `flexbox` doing the same wrapping
/// over elements that already carry it -- word by word, because a span that is
/// a whole sentence would otherwise be moved to the next line as a unit.
Element wrapped(const std::vector<markdown::Span>& spans, bool dim_all) {
    Elements words;
    for (const markdown::Span& span : spans) {
        std::size_t at = 0;
        while (at < span.text.size()) {
            const std::size_t space = span.text.find(' ', at);
            const std::string word  = span.text.substr(
                at, space == std::string::npos ? std::string::npos : space - at);
            if (!word.empty()) {
                words.push_back(span_element({word, span.code, span.bold, span.italic}, dim_all));
            }
            if (space == std::string::npos) {
                break;
            }
            // The space is its own element, so the wrap can break on it.
            words.push_back(text(" "));
            at = space + 1;
        }
    }
    if (words.empty()) {
        return text("");
    }
    return flexbox(std::move(words), FlexboxConfig());
}

/// One table cell, aligned as the delimiter row asked.
Element table_cell(const std::vector<markdown::Span>& spans, char alignment, bool dim_all) {
    Elements runs;
    for (const markdown::Span& span : spans) {
        runs.push_back(span_element(span, dim_all));
    }
    if (runs.empty()) {
        runs.push_back(text(""));
    }
    Element cell = hbox(std::move(runs));
    // A space either side, because FTXUI draws the separators hard against the
    // content and a column of numbers touching a line is harder to read than
    // one that is not.
    switch (alignment) {
        case 'r': return hbox({text(" "), filler(), std::move(cell), text(" ")});
        case 'c': return hbox({text(" "), filler(), std::move(cell), filler(), text(" ")});
        default:  return hbox({text(" "), std::move(cell), filler(), text(" ")});
    }
}

/// The bullet drawn for a list item at `level`.
std::string bullet_for(int level) {
    switch (level % 3) {
        case 0:  return "• ";
        case 1:  return "◦ ";
        default: return "‣ ";
    }
}

}  // namespace

std::vector<Element> render_markdown(const std::string& source, bool dim_all) {
    std::vector<Element> lines;
    bool previous_blank = true;  // suppresses a leading gap

    const std::vector<markdown::Block> blocks = markdown::parse(source);
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const markdown::Block& block = blocks[index];
        const std::string pad(static_cast<std::size_t>(block.level) * 2, ' ');

        // A table is many blocks and one element, so it is gathered here rather
        // than drawn a row at a time. FTXUI's Table sizes the columns to their
        // contents, which is the whole reason a table is worth drawing rather
        // than printing: the pipes a model writes do not line anything up.
        if (block.kind == markdown::BlockKind::TableRow) {
            std::string alignment;
            std::vector<std::vector<Element>> rows;
            std::size_t end = index;
            for (; end < blocks.size(); ++end) {
                if (blocks[end].kind == markdown::BlockKind::TableRule) {
                    alignment = blocks[end].marker;
                    continue;
                }
                if (blocks[end].kind != markdown::BlockKind::TableRow) {
                    break;
                }
                std::vector<Element> row;
                for (std::size_t cell = 0; cell < blocks[end].cells.size(); ++cell) {
                    const char align = cell < alignment.size() ? alignment[cell] : 'l';
                    row.push_back(table_cell(blocks[end].cells[cell], align, dim_all));
                }
                rows.push_back(std::move(row));
            }

            const bool has_header = !rows.empty();
            Table table(std::move(rows));
            table.SelectAll().Border(LIGHT);
            table.SelectAll().SeparatorVertical(LIGHT);
            if (has_header) {
                // The header is what makes a table readable at a glance, and a
                // rule under it is what separates it from the data.
                table.SelectRow(0).Decorate(bold);
                table.SelectRow(0).Border(LIGHT);
            }
            if (dim_all) {
                table.SelectAll().Decorate(color(theme::kMeta));
                table.SelectAll().Decorate(dim);
            }
            lines.push_back(table.Render());
            previous_blank = false;
            index = end - 1;
            continue;
        }

        switch (block.kind) {
            case markdown::BlockKind::Blank:
                // One gap, however many blank lines the model left.
                if (!previous_blank) {
                    lines.push_back(text(""));
                }
                previous_blank = true;
                continue;

            case markdown::BlockKind::Rule:
                lines.push_back(separatorLight() | color(theme::kMeta));
                break;

            case markdown::BlockKind::Heading: {
                // Every level is bold; the first two are also colored, so a
                // document with headings four deep still reads as a hierarchy.
                Element line = wrapped(block.spans, dim_all) | bold;
                if (!dim_all && block.level <= 2) {
                    line = line | color(theme::kHeading);
                }
                lines.push_back(std::move(line));
                break;
            }

            case markdown::BlockKind::Bullet:
                lines.push_back(hbox({
                    text(pad + bullet_for(block.level)) | color(dim_all ? theme::kMeta
                                                                       : theme::kMarker),
                    wrapped(block.spans, dim_all) | flex,
                }));
                break;

            case markdown::BlockKind::Numbered:
                lines.push_back(hbox({
                    text(pad + block.marker + " ") | color(dim_all ? theme::kMeta
                                                                  : theme::kMarker),
                    wrapped(block.spans, dim_all) | flex,
                }));
                break;

            case markdown::BlockKind::Quote:
                lines.push_back(hbox({
                    text("│ ") | color(theme::kMeta),
                    wrapped(block.spans, dim_all) | color(theme::kMeta) | flex,
                }));
                break;

            case markdown::BlockKind::Code: {
                // Gathered back into one block rather than drawn a line at a
                // time. A code block is one thing -- it has a language, a
                // length, and line numbers that count from its own top -- and
                // none of that can be worked out from a single row.
                //
                // Not wrapped and not word-split either: code that is re-flowed
                // is code that no longer runs. A long line scrolls off, which
                // is the honest failure.
                std::size_t last = index;
                std::string body;
                for (; last < blocks.size() && blocks[last].kind == markdown::BlockKind::Code;
                     ++last) {
                    if (last > index) {
                        body += '\n';
                    }
                    body += blocks[last].spans.empty() ? std::string()
                                                       : blocks[last].spans.front().text;
                }
                lines.push_back(code_block_impl(body, block.marker, dim_all));
                index = last - 1;
                previous_blank = false;
                continue;
            }

            case markdown::BlockKind::TableRow:
            case markdown::BlockKind::TableRule:
                break;  // gathered above

            case markdown::BlockKind::Paragraph:
                lines.push_back(hbox({
                    text(pad),
                    wrapped(block.spans, dim_all) | flex,
                }));
                break;
        }
        previous_blank = false;
    }

    return lines;
}

Element code_listing(const std::string& body, const std::string& language, bool dim) {
    return code_block_impl(body, language, dim);
}

}  // namespace crucible::ui
