// SPDX-License-Identifier: MIT
//
// See zip.hpp. The format is PKWARE's APPNOTE: a local header before each
// file's bytes, a central directory after all of them that says where each
// one is, and a record at the very end that says where the directory is.
#include "crucible/util/zip.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>

#include <miniz.h>

namespace crucible::util::zip {
namespace {

void put16(std::string& out, std::uint32_t value) {
    out += static_cast<char>(value & 0xFFU);
    out += static_cast<char>((value >> 8) & 0xFFU);
}

void put32(std::string& out, std::uint32_t value) {
    put16(out, value & 0xFFFFU);
    put16(out, (value >> 16) & 0xFFFFU);
}

/// When `file` was last written, as the two 16-bit fields an archive keeps
/// it in: the time to two seconds, and the date from 1980. Local time, which
/// is what every archive program shows.
void dos_time(const std::filesystem::path& file, std::uint32_t& time, std::uint32_t& date) {
    std::error_code ec;
    const auto      written = std::filesystem::last_write_time(file, ec);
    std::time_t     when    = std::time(nullptr);
    if (!ec) {
        // file_time_type's clock is not the system's everywhere, and
        // clock_cast is not in every standard library yet: measured against
        // both clocks' now instead.
        const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            written - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        when = std::chrono::system_clock::to_time_t(system);
    }
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &when);
#else
    localtime_r(&when, &parts);
#endif
    const int year = std::max(parts.tm_year + 1900, 1980);
    time = static_cast<std::uint32_t>((parts.tm_hour << 11) | (parts.tm_min << 5) | (parts.tm_sec / 2));
    date = static_cast<std::uint32_t>(((year - 1980) << 9) | ((parts.tm_mon + 1) << 5) | parts.tm_mday);
}

/// Past this, sizes and offsets need zip64.
constexpr std::uint64_t kMost = 0xFFFFFFFFULL;

/// Whether `name` is a kind of file that is compressed already: pictures,
/// recordings, films and archives.
bool packed_already(const std::string& name) {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* one : {"png", "jpg", "jpeg", "gif", "webp", "avif", "heic", "mp3", "m4a", "aac", "ogg", "opus",
                            "flac", "mp4", "m4v", "mov", "webm", "mkv", "zip", "gz", "tgz", "bz2", "xz", "7z", "zst",
                            "jar", "whl", "apk", "docx", "xlsx", "pptx", "woff", "woff2"}) {
        if (ext == one) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string write(const std::filesystem::path& target, const std::vector<Entry>& entries) {
    if (entries.size() > 0xFFFFU) {
        return "more than 65535 files";
    }
    std::filesystem::path partial = target;
    partial += ".part";
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    if (!out) {
        return "could not write " + partial.string();
    }
    std::string    directory;
    std::uint64_t  offset = 0;
    const auto     fail   = [&](std::string why) {
        out.close();
        std::error_code ec;
        std::filesystem::remove(partial, ec);
        return why;
    };
    for (const Entry& entry : entries) {
        // Measured before it is read: a model file in a project is tens of
        // gigabytes, and is refused here rather than read into memory first.
        std::error_code   measured;
        const std::uintmax_t size = std::filesystem::file_size(entry.file, measured);
        if (!measured && (size >= kMost || offset + size >= kMost)) {
            return fail("larger than 4 GB");
        }
        std::ifstream in(entry.file, std::ios::binary);
        if (!in) {
            return fail("could not read " + entry.file.string());
        }
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.size() >= kMost || offset + bytes.size() >= kMost) {
            return fail("larger than 4 GB");
        }
        const auto crc = static_cast<std::uint32_t>(
            mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
        // Raw deflate, the kind an archive holds: no zlib header around it.
        // Not for what is packed already, which only gets slower to save.
        std::size_t packed_size = 0;
        void*       packed      = bytes.empty() || packed_already(entry.name)
                                    ? nullptr
                                    : tdefl_compress_mem_to_heap(bytes.data(), bytes.size(), &packed_size,
                                                                 TDEFL_DEFAULT_MAX_PROBES);
        const bool        deflated = packed != nullptr && packed_size < bytes.size();
        const std::string body     = deflated ? std::string(static_cast<const char*>(packed), packed_size) : bytes;
        mz_free(packed);

        std::uint32_t time = 0;
        std::uint32_t date = 0;
        dos_time(entry.file, time, date);
        const std::uint32_t method = deflated ? 8 : 0;

        std::string head;
        put32(head, 0x04034b50U);
        put16(head, 20);        // version needed: deflate
        put16(head, 0x0800U);   // the name is UTF-8
        put16(head, method);
        put16(head, time);
        put16(head, date);
        put32(head, crc);
        put32(head, static_cast<std::uint32_t>(body.size()));
        put32(head, static_cast<std::uint32_t>(bytes.size()));
        put16(head, static_cast<std::uint32_t>(entry.name.size()));
        put16(head, 0);         // no extra field
        head += entry.name;
        out.write(head.data(), static_cast<std::streamsize>(head.size()));
        out.write(body.data(), static_cast<std::streamsize>(body.size()));

        // Made on Unix (3), so the permissions in the high half of the
        // external attributes are read: a script stays executable.
        std::error_code ec;
        const auto      perms = static_cast<std::uint32_t>(std::filesystem::status(entry.file, ec).permissions())
                         & 0777U;
        put32(directory, 0x02014b50U);
        put16(directory, (3U << 8) | 20U);
        put16(directory, 20);
        put16(directory, 0x0800U);
        put16(directory, method);
        put16(directory, time);
        put16(directory, date);
        put32(directory, crc);
        put32(directory, static_cast<std::uint32_t>(body.size()));
        put32(directory, static_cast<std::uint32_t>(bytes.size()));
        put16(directory, static_cast<std::uint32_t>(entry.name.size()));
        put16(directory, 0);    // extra
        put16(directory, 0);    // comment
        put16(directory, 0);    // disk
        put16(directory, 0);    // internal attributes
        put32(directory, (0100000U | (ec ? 0644U : perms)) << 16);   // a regular file, and its mode
        put32(directory, static_cast<std::uint32_t>(offset));
        directory += entry.name;
        offset += head.size() + body.size();
    }
    if (offset + directory.size() >= kMost) {
        return fail("larger than 4 GB");
    }
    std::string end;
    put32(end, 0x06054b50U);
    put16(end, 0);   // this disk
    put16(end, 0);   // the disk the directory starts on
    put16(end, static_cast<std::uint32_t>(entries.size()));
    put16(end, static_cast<std::uint32_t>(entries.size()));
    put32(end, static_cast<std::uint32_t>(directory.size()));
    put32(end, static_cast<std::uint32_t>(offset));
    put16(end, 0);   // no comment
    out.write(directory.data(), static_cast<std::streamsize>(directory.size()));
    out.write(end.data(), static_cast<std::streamsize>(end.size()));
    out.close();
    if (!out) {
        return fail("could not finish writing " + partial.string());
    }
    std::error_code ec;
    std::filesystem::rename(partial, target, ec);
    if (ec) {
        return fail("could not put it at " + target.string() + ": " + ec.message());
    }
    return {};
}

}  // namespace crucible::util::zip
