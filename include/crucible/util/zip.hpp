// SPDX-License-Identifier: MIT
//
// A folder's files as one .zip, for a build downloaded from the shelf.
//
// miniz is compiled in to read archives (see attachments.cpp) and without its
// writer, which brings time functions that differ between platforms with it.
// Writing one is a few headers around each file's deflated bytes, so it is
// written here instead, with miniz's own deflate and its CRC: every archive
// program reads what this writes, and the times on its files are the files'
// own.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace crucible::util::zip {

/// One file to put in an archive: where it is, and its name inside the
/// archive, with forward slashes ("src/app.js").
struct Entry {
    std::filesystem::path file;
    std::string           name;
};

/// Write `entries` into a new archive at `target`, each deflated, or stored
/// as it is where deflating makes it no smaller. Written beside `target`
/// first and moved into place once whole, so a failure leaves nothing
/// behind. Empty when it was written, and otherwise why not: an archive past
/// 4 GB, or with more than 65535 files, needs zip64, which this does not
/// write.
std::string write(const std::filesystem::path& target, const std::vector<Entry>& entries);

}  // namespace crucible::util::zip
