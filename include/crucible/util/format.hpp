// SPDX-License-Identifier: MIT
//
// Turning numbers and strings into things a person reads at a glance.
//
// Small, but worth having in one place: three separate byte formatters had
// drifted apart across the codebase before this existed.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace crucible::format {

/// Strip leading and trailing whitespace.
std::string trim(std::string text);

/// The same text in lower case, ASCII only.
///
/// For comparing things a person typed or a file is called -- an extension, a
/// keyword, a provider's name -- where "GGUF" and "gguf" are the same answer.
/// Not for prose: it leaves every non-ASCII letter as it found it.
std::string to_lower(std::string_view text);

/// A double at fixed precision: `number(18.34567, 1)` is `"18.3"`.
std::string number(double value, int precision);

/// Milliseconds as a duration a person reads: `"840ms"`, `"3.2s"`.
std::string duration_ms(long milliseconds);

/// A byte count in the largest unit that keeps it readable: `"469 MB"`,
/// `"1.2 GB"`. Whole numbers below a kilobyte.
std::string bytes(std::uintmax_t count);

/// A path with the home directory written as `~`.
///
/// Panel titles sit beside the directory they are about, and Crucible's own
/// directories are four levels below $HOME -- spelling one out in full pushes
/// the title off its own header on any normal terminal.
std::string short_path(const std::filesystem::path& path);

/// Bytes as base64, padded: what a data: URI and a picture sent to a model's
/// API both carry.
std::string base64(std::string_view bytes);

/// base64 back to bytes. Padding is optional and white space is skipped;
/// false when anything else is not part of the alphabet.
bool from_base64(std::string_view text, std::string& out);

}  // namespace crucible::format
