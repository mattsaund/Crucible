// SPDX-License-Identifier: MIT
//
// See markdown.hpp.
#include "crucible/util/markdown.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace crucible::markdown {
namespace {

/// How many spaces a line begins with, counting a tab as four.
int indent_of(std::string_view line) {
    int spaces = 0;
    for (const char c : line) {
        if (c == ' ')       { ++spaces; }
        else if (c == '\t') { spaces += 4; }
        else                { break; }
    }
    return spaces;
}

std::string_view lstrip(std::string_view line) {
    while (!line.empty() && (std::isspace(static_cast<unsigned char>(line.front())) != 0)) {
        line.remove_prefix(1);
    }
    return line;
}

std::string_view rstrip(std::string_view line) {
    while (!line.empty() && (std::isspace(static_cast<unsigned char>(line.back())) != 0)) {
        line.remove_suffix(1);
    }
    return line;
}

/// `- `, `* ` or `+ ` at the start of a line, but not `**bold**` and not a
/// `---` rule, both of which begin with the same characters.
bool is_bullet(std::string_view line) {
    if (line.size() < 2) {
        return false;
    }
    const char marker = line.front();
    if (marker != '-' && marker != '*' && marker != '+') {
        return false;
    }
    return line[1] == ' ';
}

/// `1.` or `2)` followed by a space. Returns the marker, or empty.
std::string numbered_marker(std::string_view line) {
    std::size_t digits = 0;
    while (digits < line.size() && (std::isdigit(static_cast<unsigned char>(line[digits])) != 0)) {
        ++digits;
    }
    if (digits == 0 || digits + 1 >= line.size()) {
        return {};
    }
    if (line[digits] != '.' && line[digits] != ')') {
        return {};
    }
    if (line[digits + 1] != ' ') {
        return {};
    }
    return std::string(line.substr(0, digits + 1));
}

/// Three or more of the same character, and nothing else. `***` is a rule;
/// `**bold**` is not, which is why the whole line has to match.
bool is_rule(std::string_view line) {
    if (line.size() < 3) {
        return false;
    }
    const char c = line.front();
    if (c != '-' && c != '*' && c != '_') {
        return false;
    }
    return std::all_of(line.begin(), line.end(), [c](char other) { return other == c; });
}

/// Split a table line into its cells, dropping the optional outer pipes.
/// Returns nothing when the line is not shaped like a row.
std::vector<std::string_view> table_cells(std::string_view line) {
    if (line.find('|') == std::string_view::npos) {
        return {};
    }
    if (!line.empty() && line.front() == '|') {
        line.remove_prefix(1);
    }
    if (!line.empty() && line.back() == '|') {
        line.remove_suffix(1);
    }

    std::vector<std::string_view> cells;
    std::size_t at = 0;
    while (at <= line.size()) {
        const std::size_t bar = line.find('|', at);
        cells.push_back(rstrip(lstrip(line.substr(
            at, bar == std::string_view::npos ? std::string_view::npos : bar - at))));
        if (bar == std::string_view::npos) {
            break;
        }
        at = bar + 1;
    }
    return cells.size() >= 2 ? cells : std::vector<std::string_view>{};
}

/// Is this the `|---|:--:|` row that turns the line above it into a header?
/// Returns the alignment of each column -- l, c or r -- or nothing.
std::string table_alignment(std::string_view line) {
    const std::vector<std::string_view> cells = table_cells(line);
    if (cells.empty()) {
        return {};
    }
    std::string alignment;
    for (std::string_view cell : cells) {
        const bool left  = !cell.empty() && cell.front() == ':';
        const bool right = !cell.empty() && cell.back() == ':';
        if (left)  { cell.remove_prefix(1); }
        if (right && !cell.empty()) { cell.remove_suffix(1); }
        if (cell.empty() ||
            !std::all_of(cell.begin(), cell.end(), [](char c) { return c == '-'; })) {
            return {};
        }
        alignment += left && right ? 'c' : (right ? 'r' : 'l');
    }
    return alignment;
}

/// The fence's language, or "" for a bare fence. Returns false if `line` is not
/// a fence at all.
bool is_fence(std::string_view line, std::string& language) {
    if (line.substr(0, 3) != "```" && line.substr(0, 3) != "~~~") {
        return false;
    }
    language = std::string(rstrip(lstrip(line.substr(3))));
    return true;
}

void push_span(std::vector<Span>& spans, std::string text, bool code, bool bold, bool italic) {
    if (text.empty()) {
        return;
    }
    // Runs of the same style are joined, so a line does not become one span per
    // character when the styling does not actually change.
    if (!spans.empty() && spans.back().code == code && spans.back().bold == bold &&
        spans.back().italic == italic) {
        spans.back().text += text;
        return;
    }
    spans.push_back({std::move(text), code, bold, italic});
}

/// Is there a closing `marker` later in `line`, starting from `from`?
///
/// Without this check an unmatched asterisk -- a multiplication sign, a
/// footnote -- would turn the rest of the line italic and swallow its own
/// character.
bool closes(std::string_view line, std::size_t from, std::string_view marker) {
    return line.find(marker, from) != std::string_view::npos;
}

}  // namespace

std::vector<Span> parse_inline(std::string_view line) {
    std::vector<Span> spans;
    std::string       pending;
    bool bold   = false;
    bool italic = false;

    for (std::size_t at = 0; at < line.size();) {
        // Inline code wins over everything: what is inside it is text, not
        // markup, which is the whole point of writing it that way.
        if (line[at] == '`' && closes(line, at + 1, "`")) {
            push_span(spans, std::exchange(pending, {}), false, bold, italic);
            const std::size_t end = line.find('`', at + 1);
            push_span(spans, std::string(line.substr(at + 1, end - at - 1)), true, bold, italic);
            at = end + 1;
            continue;
        }
        if (line.substr(at, 2) == "**" && (bold || closes(line, at + 2, "**"))) {
            push_span(spans, std::exchange(pending, {}), false, bold, italic);
            bold = !bold;
            at  += 2;
            continue;
        }
        if ((line[at] == '*' || line[at] == '_') &&
            (italic || closes(line, at + 1, line.substr(at, 1)))) {
            // `_` only between non-word characters: snake_case names are far
            // more common in an expert's answer than underscore emphasis.
            const bool wordish =
                line[at] == '_' &&
                ((at > 0 && std::isalnum(static_cast<unsigned char>(line[at - 1])) != 0) ||
                 (at + 1 < line.size() &&
                  std::isalnum(static_cast<unsigned char>(line[at + 1])) != 0 && !italic));
            if (!wordish) {
                push_span(spans, std::exchange(pending, {}), false, bold, italic);
                italic = !italic;
                at    += 1;
                continue;
            }
        }
        pending += line[at];
        ++at;
    }
    push_span(spans, std::move(pending), false, bold, italic);
    return spans;
}

std::vector<Block> parse(std::string_view text) {
    // A trailing newline terminates the last line rather than starting an
    // empty one, so it is dropped before splitting. Without this every reply
    // ends with a blank block and every reply is drawn with a gap under it.
    if (!text.empty() && text.back() == '\n') {
        text.remove_suffix(1);
    }

    // Split first, because a table cannot be recognized a line at a time: what
    // makes a row of pipes a table is the `|---|` under it, and that is the
    // next line.
    std::vector<std::string_view> lines;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t end = text.find('\n', at);
        lines.push_back(
            text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        if (end == std::string_view::npos) {
            break;
        }
        at = end + 1;
    }

    std::vector<Block> blocks;
    bool        in_code = false;
    bool        in_table = false;
    std::string language;

    const auto row_block = [](std::string_view line) {
        Block block;
        block.kind = BlockKind::TableRow;
        for (const std::string_view cell : table_cells(line)) {
            block.cells.push_back(parse_inline(cell));
        }
        return block;
    };

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const std::string_view raw = lines[index];

        std::string fence_language;
        if (is_fence(lstrip(raw), fence_language)) {
            if (in_code) {
                in_code = false;
            } else {
                in_code  = true;
                language = fence_language;
            }
            in_table = false;
            continue;  // the fence itself is not a line of output
        }
        if (in_code) {
            // Verbatim, including the indentation: it is code.
            blocks.push_back({BlockKind::Code, 0, language,
                              {{std::string(raw), true, false, false}}, {}});
            continue;
        }

        const std::string_view line = rstrip(raw);
        const std::string_view body = lstrip(line);

        // --- tables ---------------------------------------------------------
        //
        // A line of pipes is only a table if the line under it is the delimiter
        // row; otherwise it is prose that happens to contain a pipe, which in
        // an answer about shell commands is not rare.
        if (in_table) {
            if (!table_cells(body).empty()) {
                blocks.push_back(row_block(body));
                continue;
            }
            in_table = false;
        } else if (!table_cells(body).empty() && index + 1 < lines.size()) {
            if (const std::string alignment = table_alignment(rstrip(lstrip(lines[index + 1])));
                !alignment.empty()) {
                blocks.push_back(row_block(body));
                Block rule;
                rule.kind   = BlockKind::TableRule;
                rule.marker = alignment;
                blocks.push_back(std::move(rule));
                in_table = true;
                ++index;  // the delimiter row is consumed here
                continue;
            }
        }

        if (body.empty()) {
            blocks.push_back({BlockKind::Blank, 0, {}, {}, {}});
            continue;
        }
        if (is_rule(body)) {
            blocks.push_back({BlockKind::Rule, 0, {}, {}, {}});
            continue;
        }
        if (body.front() == '#') {
            std::size_t level = 0;
            while (level < body.size() && body[level] == '#') {
                ++level;
            }
            if (level <= 6 && level < body.size() && body[level] == ' ') {
                blocks.push_back({BlockKind::Heading, static_cast<int>(level), {},
                                  parse_inline(lstrip(body.substr(level))), {}});
                continue;
            }
        }
        if (body.front() == '>') {
            blocks.push_back({BlockKind::Quote, 0, {},
                              parse_inline(lstrip(body.substr(1))), {}});
            continue;
        }
        if (is_bullet(body)) {
            blocks.push_back({BlockKind::Bullet, indent_of(line) / 2, {},
                              parse_inline(lstrip(body.substr(1))), {}});
            continue;
        }
        if (const std::string marker = numbered_marker(body); !marker.empty()) {
            blocks.push_back({BlockKind::Numbered, indent_of(line) / 2, marker,
                              parse_inline(lstrip(body.substr(marker.size()))), {}});
            continue;
        }
        blocks.push_back({BlockKind::Paragraph, indent_of(line) / 2, {},
                          parse_inline(line), {}});
    }

    // A fence the model opened and never closed leaves everything after it as
    // code, which is right: it is what the model said it was.
    return blocks;
}

}  // namespace crucible::markdown
