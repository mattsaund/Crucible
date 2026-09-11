// SPDX-License-Identifier: MIT
#include "crucible/util/code_lines.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace crucible::syntax {
namespace {

/// The line starting at `start`, and where the next one starts.
std::string_view line_at(std::string_view text, std::size_t start, std::size_t& next) {
    const std::size_t end = text.find('\n', start);
    next = end == std::string_view::npos ? std::string_view::npos : end + 1;
    return text.substr(start, end == std::string_view::npos ? end : end - start);
}

/// Read the starting line numbers out of a hunk header.
///
/// Two shapes. Git writes `@@ -12,7 +12,9 @@`, which names both sides. Crucible
/// writes `@@ line 42 @@` from util/diff.cpp, which names one -- and names the
/// first *changed* line rather than the first line of the hunk, so the context
/// rows above it have to be counted back off. `context` is how many of those
/// follow, which the caller has just looked ahead for; counting them is what
/// keeps this from having to know how much context util/diff.cpp emits.
bool parse_hunk(std::string_view line, int context, int& old_no, int& new_no) {
    const std::string text(line);
    int a = 0;
    int b = 0;
    if (std::sscanf(text.c_str(), "@@ -%d,%*d +%d,%*d @@", &a, &b) == 2
        || std::sscanf(text.c_str(), "@@ -%d +%d @@", &a, &b) == 2
        || std::sscanf(text.c_str(), "@@ -%d,%*d +%d @@", &a, &b) == 2
        || std::sscanf(text.c_str(), "@@ -%d +%d,%*d @@", &a, &b) == 2) {
        old_no = a;
        new_no = b;
        return true;
    }
    if (std::sscanf(text.c_str(), "@@ line %d @@", &a) == 1) {
        old_no = std::max(a - context, 1);
        new_no = old_no;
        return true;
    }
    return false;
}

/// How many context rows follow the hunk header, before the first line that
/// actually changed.
int context_after(std::string_view text, std::size_t after) {
    int rows = 0;
    std::size_t start = after;
    while (start < text.size()) {
        std::size_t next = 0;
        const std::string_view line = line_at(text, start, next);
        if (line.empty() || line.front() != ' ') {
            break;
        }
        ++rows;
        if (next == std::string_view::npos) {
            break;
        }
        start = next;
    }
    return rows;
}

/// A line that is part of a diff's preamble rather than one of its changes.
bool is_file_header(std::string_view line) {
    return line.rfind("+++", 0) == 0 || line.rfind("---", 0) == 0
        || line.rfind("diff ", 0) == 0 || line.rfind("index ", 0) == 0;
}

}  // namespace

std::size_t columns(std::string_view text) {
    std::size_t width = 0;
    for (const char byte : text) {
        width += (static_cast<unsigned char>(byte) & 0xC0U) != 0x80U ? 1 : 0;
    }
    return width;
}

bool looks_like_diff(std::string_view text) {
    std::size_t added   = 0;
    std::size_t removed = 0;
    std::size_t total   = 0;
    std::size_t start   = 0;
    while (start <= text.size()) {
        std::size_t next = 0;
        const std::string_view line = line_at(text, start, next);
        if (line.rfind("@@", 0) == 0 && line.find("@@", 2) != std::string_view::npos) {
            return true;
        }
        if (!line.empty() && line.front() == '+') { ++added; }
        if (!line.empty() && line.front() == '-') { ++removed; }
        ++total;
        if (next == std::string_view::npos) {
            break;
        }
        start = next;
    }
    return total > 1 && added > 0 && removed > 0 && (added + removed) * 2 >= total;
}

std::string diff_path(std::string_view text) {
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t next = 0;
        const std::string_view line = line_at(text, start, next);
        if (line.rfind("+++ ", 0) == 0) {
            std::string path(line.substr(4));
            // git writes b/src/foo.cpp; the b/ is bookkeeping, not the path.
            if (path.rfind("b/", 0) == 0) {
                path = path.substr(2);
            }
            if (const std::size_t tab = path.find('\t'); tab != std::string::npos) {
                path = path.substr(0, tab);
            }
            return path == "/dev/null" ? std::string() : path;
        }
        if (next == std::string_view::npos) {
            break;
        }
        start = next;
    }
    return {};
}

std::vector<CodeRow> code_rows(std::string_view text, bool diff, int& added,
                               int& removed) {
    std::vector<CodeRow> rows;
    int old_no = 1;
    int new_no = 1;
    added   = 0;
    removed = 0;

    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t next = 0;
        const std::string_view line = line_at(text, start, next);

        CodeRow row;
        if (!diff) {
            row.text   = std::string(line);
            row.new_no = static_cast<int>(rows.size()) + 1;
        } else if (line.rfind("@@", 0) == 0) {
            row.marker = '@';
            row.text   = std::string(line);
            const std::size_t after = next == std::string_view::npos ? text.size() : next;
            parse_hunk(line, context_after(text, after), old_no, new_no);
        } else if (is_file_header(line)) {
            // The preamble. Not a change, so it is numbered as nothing and
            // drawn like the hunk headers it sits above.
            row.marker = '@';
            row.text   = std::string(line);
        } else if (!line.empty() && line.front() == '+') {
            row.marker = '+';
            row.text   = std::string(line.substr(1));
            row.new_no = new_no++;
            ++added;
        } else if (!line.empty() && line.front() == '-') {
            row.marker = '-';
            row.text   = std::string(line.substr(1));
            row.old_no = old_no++;
            ++removed;
        } else {
            row.marker = ' ';
            row.text   = line.empty() ? std::string() : std::string(line.substr(1));
            row.old_no = old_no++;
            row.new_no = new_no++;
        }
        rows.push_back(std::move(row));

        if (next == std::string_view::npos) {
            break;
        }
        start = next;
    }
    // A trailing newline is the end of the last line, not an empty line after
    // it. Left in, every code block would gain a blank row at the bottom.
    if (rows.size() > 1 && rows.back().text.empty() && rows.back().marker != '@') {
        rows.pop_back();
    }
    return rows;
}

}  // namespace crucible::syntax
