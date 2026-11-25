// SPDX-License-Identifier: MIT
//
// The shelf: what the experts made in a conversation, kept with it.
//
// A document, a picture or an archive is a thing to download; code is part of
// the conversation's build, which is entered to read, edit and preview it.
// What a prompt made is found on disk when it ends -- a file a tool wrote and
// one a command left behind alike -- and stays on the shelf until somebody
// deletes it, or deletes the conversation.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace crucible::shelf {

/// One thing an expert made.
struct Entry {
    std::string  path;       ///< relative to the conversation's folder, with forward slashes
    std::int64_t added = 0;  ///< seconds since the epoch
};

/// Whether `path` is code, and so part of the build, rather than something to
/// download: a document, a picture, a recording, an archive or a program.
bool is_code(const std::filesystem::path& path);

/// What kind of download `path` is, for the shelf to draw: "pdf", "document",
/// "sheet", "slides", "image", "audio", "video", "archive", "text" or "file".
std::string kind_of(const std::filesystem::path& path);

/// Files under `root` last written at or after `since`, relative to it. Not
/// in version control, dependencies or caches, and only what is not code in a
/// build's output folders. At most `limit` of them.
std::vector<std::string> made_since(const std::filesystem::path& root,
                                    std::filesystem::file_time_type since, std::size_t limit = 500);

/// Add `made` to `entries`, those not on it already, stamped `now`. True when
/// anything was added.
bool merge(std::vector<Entry>& entries, const std::vector<std::string>& made, std::int64_t now);

nlohmann::json     to_json(const std::vector<Entry>& entries);
std::vector<Entry> from_json(const nlohmann::json& value);

/// This machine's downloads folder.
std::filesystem::path downloads_dir();

/// Where a download of `name` lands: the downloads folder, under a name not
/// taken there yet ("report (2).pdf").
std::filesystem::path download_target(const std::string& name);

/// `relative` under `root`, or empty when it would reach outside it.
std::filesystem::path inside(const std::filesystem::path& root, const std::string& relative);

}  // namespace crucible::shelf
