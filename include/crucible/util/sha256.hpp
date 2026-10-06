// SPDX-License-Identifier: MIT
//
// SHA-256, for checking that a download is the file it claims to be.
//
// Crucible fetches its own Python from a third party's releases, and a file
// that arrived truncated, or was swapped on the way, has to be refused before
// it is unpacked and run. The digest each one should have is compiled in
// beside its address, and this is what it is compared against.
//
// Written out rather than linked: it is a hundred lines of arithmetic from
// FIPS 180-4, and curl -- the one thing here that already speaks TLS -- is a
// separate program, not a library this could borrow it from.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace crucible::util {

class Sha256 {
public:
    Sha256();

    void update(const void* data, std::size_t size);
    void update(std::string_view text) { update(text.data(), text.size()); }

    /// The digest as 64 lowercase hex digits. Finishes the hash: call once.
    std::string hex();

private:
    void block(const std::uint8_t* chunk);

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t                  buffered_ = 0;
    std::uint64_t                length_   = 0;   ///< bytes seen
};

/// The digest of a whole file, or an empty string when it cannot be read.
std::string sha256_file(const std::filesystem::path& path);

}  // namespace crucible::util
