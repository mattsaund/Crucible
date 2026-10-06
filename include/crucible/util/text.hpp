// SPDX-License-Identifier: MIT
// Small text helpers that are worth testing on their own.
#pragma once

#include <string>
#include <string_view>

namespace crucible::detail {

/// Step one codepoint left through `text` from byte offset `index`.
/// Moving by bytes would drop a cursor into the middle of a character.
std::size_t utf8_prev(const std::string& text, std::size_t index);

/// Step one codepoint right through `text` from byte offset `index`.
std::size_t utf8_next(const std::string& text, std::size_t index);

/// Number of bytes in the UTF-8 sequence that starts with `lead`.
/// A stray continuation byte reports 1 so callers make progress instead of
/// stalling on malformed input.
int utf8_length(unsigned char lead);

/// Split `buffer` into a prefix of complete UTF-8 sequences and a remainder.
///
/// A single model token can end in the middle of a codepoint, so emitting raw
/// pieces would print replacement characters mid-word. The incomplete tail is
/// left in `buffer` until the bytes that finish it arrive.
///
/// Returns the complete prefix and erases it from `buffer`.
std::string take_complete_utf8(std::string& buffer);

/// Whether every byte of `text` is part of a well-formed UTF-8 sequence.
bool is_utf8(std::string_view text);

/// `text`, with each byte that is not part of a well-formed UTF-8 sequence
/// replaced by U+FFFD.
///
/// Everything Crucible keeps -- a session, a cook's journal, the config -- is
/// JSON, and JSON is UTF-8: a string that is not refuses to be written, and
/// refusing threw from whichever thread was saving. Text from outside -- a
/// file read into a turn, what a command printed, the end of a reply cut off
/// mid-character -- is made UTF-8 where it comes in.
std::string scrub_utf8(std::string_view text);

/// What a command printed, as UTF-8. On Windows a console program writes in
/// the console's code page -- an English `dir` puts 0xFF between thousands --
/// so output that is not already UTF-8 is read in that code page. Elsewhere it
/// is scrubbed.
std::string console_to_utf8(std::string_view text);

}  // namespace crucible::detail
