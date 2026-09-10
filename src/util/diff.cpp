// SPDX-License-Identifier: MIT
//
// What changed between two versions of a file.
//
// Not a general diff: no LCS, no move detection, one hunk. The common prefix
// and common suffix are skipped and everything between them is reported as the
// change, which for two edits far apart in a file over-reports the middle.
//
// That is the right trade here. This exists to answer "what did that write
// do?" in a journal entry, where the answer is almost always one contiguous
// edit, and an exact algorithm would cost more than the accuracy is worth. A
// diff that has to be truncated says so in place: a diff that stops without
// warning reads as a complete diff of a smaller change.
#include "crucible/util/diff.hpp"

#include <algorithm>
#include <sstream>
#include <vector>

namespace crucible::util {
namespace {

std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> lines;
    std::string              line;
    std::istringstream       stream{std::string(text)};
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

/// The span of `before` and `after` that actually differs.
///
/// Everything before `head` is identical and everything after the tail is too,
/// so the change is whatever is left in the middle. `head` never runs past the
/// end of either side, and the two tails never overlap their own head -- which
/// is the whole of the arithmetic that makes this correct on an insertion at
/// the very start or the very end.
struct Span {
    std::size_t head = 0;       ///< first differing index, in both
    std::size_t before_end = 0; ///< one past the last differing line of `before`
    std::size_t after_end = 0;  ///< and of `after`
};

Span differing_span(const std::vector<std::string>& before,
                    const std::vector<std::string>& after) {
    Span span;
    const std::size_t shortest = std::min(before.size(), after.size());
    while (span.head < shortest && before[span.head] == after[span.head]) {
        ++span.head;
    }

    std::size_t tail = 0;
    while (tail < shortest - span.head
           && before[before.size() - 1 - tail] == after[after.size() - 1 - tail]) {
        ++tail;
    }
    span.before_end = before.size() - tail;
    span.after_end  = after.size() - tail;
    return span;
}

}  // namespace

std::string DiffStat::summary() const {
    if (added == 0 && removed == 0) {
        return "unchanged";
    }
    std::string out;
    if (added > 0) {
        out += "+" + std::to_string(added);
    }
    if (removed > 0) {
        out += (out.empty() ? "" : " ") + std::string("-") + std::to_string(removed);
    }
    return out;
}

DiffStat diff_stat(std::string_view before, std::string_view after) {
    const std::vector<std::string> old_lines = lines_of(before);
    const std::vector<std::string> new_lines = lines_of(after);
    const Span span = differing_span(old_lines, new_lines);

    DiffStat stat;
    stat.removed = static_cast<int>(span.before_end - span.head);
    stat.added   = static_cast<int>(span.after_end - span.head);
    return stat;
}

ChangedLines changed_lines(std::string_view before, std::string_view after) {
    const std::vector<std::string> old_lines = lines_of(before);
    const std::vector<std::string> new_lines = lines_of(after);
    const Span span = differing_span(old_lines, new_lines);

    ChangedLines changed;
    changed.first      = span.head;
    changed.before_end = span.before_end;
    changed.after_end  = span.after_end;
    return changed;
}

std::string unified_diff(std::string_view before, std::string_view after,
                         std::size_t max_lines) {
    const std::vector<std::string> old_lines = lines_of(before);
    const std::vector<std::string> new_lines = lines_of(after);
    const Span span = differing_span(old_lines, new_lines);

    if (span.head == span.before_end && span.head == span.after_end) {
        return {};  // identical
    }

    // Two lines of context either side, which is enough to place a hunk in a
    // file without turning the journal into a copy of it.
    constexpr std::size_t kContext = 2;
    const std::size_t from = span.head > kContext ? span.head - kContext : 0;

    std::string out;
    std::size_t emitted = 0;
    const auto emit = [&](char marker, const std::string& line) {
        if (emitted >= max_lines) {
            return false;
        }
        out += marker;
        out += line;
        out += '\n';
        ++emitted;
        return true;
    };

    out += "@@ line " + std::to_string(span.head + 1) + " @@\n";
    for (std::size_t i = from; i < span.head; ++i) {
        emit(' ', old_lines[i]);
    }
    bool truncated = false;
    for (std::size_t i = span.head; i < span.before_end; ++i) {
        if (!emit('-', old_lines[i])) {
            truncated = true;
            break;
        }
    }
    for (std::size_t i = span.head; i < span.after_end; ++i) {
        if (!emit('+', new_lines[i])) {
            truncated = true;
            break;
        }
    }
    for (std::size_t i = span.after_end;
         i < std::min(span.after_end + kContext, new_lines.size()); ++i) {
        emit(' ', new_lines[i]);
    }

    if (truncated) {
        // Said in place. A diff that stops without warning reads as a complete
        // diff of a smaller change, which is a worse lie than no diff at all.
        const DiffStat stat = diff_stat(before, after);
        out += "... (" + stat.summary() + " in total; the rest is not shown)\n";
    }
    return out;
}

}  // namespace crucible::util
