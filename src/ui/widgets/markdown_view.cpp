// SPDX-License-Identifier: MIT
//
// See markdown_view.hpp.
#include "crucible/ui/widgets/markdown_view.hpp"

#include <string>

#include "crucible/ui/theme.hpp"
#include <ftxui/dom/table.hpp>

#include "crucible/util/markdown.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace)

namespace crucible::ui {
namespace {

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

            case markdown::BlockKind::Code:
                // Not wrapped, and not word-split: code that is re-flowed is
                // code that no longer runs. A long line scrolls off, which is
                // the honest failure.
                lines.push_back(hbox({
                    text("  ") ,
                    text(block.spans.empty() ? std::string() : block.spans.front().text)
                        | color(theme::kCode),
                }));
                break;

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

}  // namespace crucible::ui
