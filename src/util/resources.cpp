// SPDX-License-Identifier: MIT
//
// See resources.hpp.
#include "crucible/util/resources.hpp"

#include <array>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include "crucible/util/subprocess.hpp"

namespace crucible::util {
namespace {

constexpr std::uint64_t kKib = 1024;

std::string_view trim(std::string_view text) {
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.front())) != 0)) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.back())) != 0)) {
        text.remove_suffix(1);
    }
    return text;
}

/// A whole number out of `text`, or nothing.
bool to_number(std::string_view text, std::uint64_t& value) {
    text = trim(text);
    if (text.empty()) {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc();
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

#if !defined(_WIN32)
/// One `sysctl -n <name>`, as a string. Empty on anything but macOS, and on a
/// Mac where the key does not exist.
std::string sysctl_value(const char* name) {
    if (!on_path("sysctl")) {
        return {};
    }
    Subprocess child;
    std::string error;
    if (!child.start({"sysctl", "-n", name}, {}, /*extra_env=*/{}, error)) {
        return {};
    }
    std::string line;
    std::string first;
    if (child.read_line(line)) {
        first = line;
    }
    while (child.read_line(line)) {
        // drain
    }
    return child.wait() == 0 ? std::string(trim(first)) : std::string{};
}

/// What one program wrote, as a whole string.
std::string output_of(const std::vector<std::string>& argv) {
    if (!on_path(argv.front())) {
        return {};
    }
    Subprocess child;
    std::string error;
    if (!child.start(argv, {}, /*extra_env=*/{}, error)) {
        return {};
    }
    std::string text;
    std::string line;
    while (child.read_line(line)) {
        text += line;
        text += '\n';
    }
    return child.wait() == 0 ? text : std::string{};
}
#endif

}  // namespace

bool parse_vm_stat(std::string_view text, std::uint64_t page_size, std::uint64_t total,
                   std::uint64_t& used) {
    // "Pages free:   123456."  -- the trailing full stop is part of the format.
    std::uint64_t free_pages    = 0;
    std::uint64_t speculative   = 0;
    std::uint64_t purgeable     = 0;
    bool          saw_something = false;

    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t end = text.find('\n', at);
        const std::string_view line =
            text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        at = end == std::string_view::npos ? text.size() + 1 : end + 1;

        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        std::string_view key   = trim(line.substr(0, colon));
        std::string_view value = trim(line.substr(colon + 1));
        while (!value.empty() && value.back() == '.') {
            value.remove_suffix(1);
        }
        std::uint64_t pages = 0;
        if (!to_number(value, pages)) {
            continue;
        }
        if (key == "Pages free")        { free_pages  = pages; saw_something = true; }
        if (key == "Pages speculative") { speculative = pages; saw_something = true; }
        if (key == "Pages purgeable")   { purgeable   = pages; saw_something = true; }
    }
    if (!saw_something || total == 0) {
        return false;
    }

    // Free, speculative and purgeable are all available without evicting
    // anything anyone wants -- the same three the Activity Monitor leaves out
    // of "memory used", and the same distinction MemAvailable draws on Linux.
    const std::uint64_t available = (free_pages + speculative + purgeable) * page_size;
    used = available < total ? total - available : 0;
    return true;
}

bool system_memory(std::uint64_t& used, std::uint64_t& total) {
    used = total = 0;
    if (parse_meminfo(read_file("/proc/meminfo"), used, total)) {
        return true;
    }
#if defined(_WIN32)
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (::GlobalMemoryStatusEx(&memory) != 0) {
        total = memory.ullTotalPhys;
        used  = memory.ullTotalPhys - memory.ullAvailPhys;
        return true;
    }
    return false;
#else
    if (!to_number(sysctl_value("hw.memsize"), total) || total == 0) {
        return false;
    }
    std::uint64_t page_size = 0;
    if (!to_number(sysctl_value("hw.pagesize"), page_size) || page_size == 0) {
        page_size = 4096;
    }
    return parse_vm_stat(output_of({"vm_stat"}), page_size, total, used);
#endif
}

int ResourceSample::memory_percent() const {
    if (total == 0) {
        return -1;
    }
    return static_cast<int>((used * 100 + total / 2) / total);
}

bool parse_gpu_line(std::string_view line, ResourceSample& into) {
    std::array<std::string_view, 5> fields{};
    std::size_t count = 0;
    std::size_t at    = 0;
    while (count < fields.size() && at <= line.size()) {
        const std::size_t comma = line.find(',', at);
        fields[count++] = trim(line.substr(at, comma == std::string_view::npos
                                                   ? std::string_view::npos
                                                   : comma - at));
        if (comma == std::string_view::npos) {
            break;
        }
        at = comma + 1;
    }
    if (count < 3 || fields[0].empty()) {
        return false;
    }

    std::uint64_t used_mb  = 0;
    std::uint64_t total_mb = 0;
    if (!to_number(fields[1], used_mb) || !to_number(fields[2], total_mb)) {
        return false;
    }

    into.name  = std::string(fields[0]);
    into.used  = used_mb * kKib * kKib;
    into.total = total_mb * kKib * kKib;

    // "[N/A]" for either of these on a card that does not report it, which is
    // a fact about the card rather than a parse failure.
    std::uint64_t number = 0;
    into.temperature_c = count > 3 && to_number(fields[3], number) ? static_cast<int>(number) : -1;
    into.busy_percent  = count > 4 && to_number(fields[4], number) ? static_cast<int>(number) : -1;
    return true;
}

bool parse_meminfo(std::string_view text, std::uint64_t& used, std::uint64_t& total) {
    std::uint64_t mem_total     = 0;
    std::uint64_t mem_available = 0;

    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t end = text.find('\n', at);
        const std::string_view line =
            text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        at = end == std::string_view::npos ? text.size() + 1 : end + 1;

        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        const std::string_view key = line.substr(0, colon);
        std::string_view value = trim(line.substr(colon + 1));
        // "16384 kB" -- the unit is always kB where it is present at all.
        if (const std::size_t space = value.find(' '); space != std::string_view::npos) {
            value = value.substr(0, space);
        }
        std::uint64_t number = 0;
        if (!to_number(value, number)) {
            continue;
        }
        if (key == "MemTotal")     { mem_total     = number * kKib; }
        if (key == "MemAvailable") { mem_available = number * kKib; }
    }

    if (mem_total == 0) {
        return false;
    }
    total = mem_total;
    used  = mem_available < mem_total ? mem_total - mem_available : 0;
    return true;
}

bool parse_stat(std::string_view text, std::uint64_t& busy, std::uint64_t& total) {
    const std::size_t end = text.find('\n');
    const std::string_view line =
        text.substr(0, end == std::string_view::npos ? std::string_view::npos : end);
    if (line.substr(0, 4) != "cpu ") {
        return false;
    }

    // user nice system idle iowait irq softirq steal guest guest_nice.
    // Idle and iowait are the two that are not work.
    std::uint64_t sum  = 0;
    std::uint64_t idle = 0;
    std::size_t   field = 0;
    std::size_t   at    = 4;
    while (at < line.size()) {
        while (at < line.size() && line[at] == ' ') {
            ++at;
        }
        const std::size_t space = line.find(' ', at);
        const std::string_view value =
            line.substr(at, space == std::string_view::npos ? std::string_view::npos : space - at);
        std::uint64_t number = 0;
        if (!value.empty() && to_number(value, number)) {
            sum += number;
            if (field == 3 || field == 4) {
                idle += number;
            }
            ++field;
        }
        if (space == std::string_view::npos) {
            break;
        }
        at = space + 1;
    }
    if (field < 4) {
        return false;
    }
    total = sum;
    busy  = sum - idle;
    return true;
}

std::string parse_cpu_name(std::string_view cpuinfo) {
    std::size_t at = 0;
    while (at <= cpuinfo.size()) {
        const std::size_t end = cpuinfo.find('\n', at);
        const std::string_view line =
            cpuinfo.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        at = end == std::string_view::npos ? cpuinfo.size() + 1 : end + 1;

        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || trim(line.substr(0, colon)) != "model name") {
            continue;
        }

        std::string name(trim(line.substr(colon + 1)));
        // The trademark furniture is not a label. "12th Gen Intel(R) Core(TM)
        // i5-12400" is thirty-four columns of which nine identify the part.
        for (const std::string_view noise : {"(R)", "(TM)", "(tm)", "CPU", "Processor"}) {
            for (std::size_t found = name.find(noise); found != std::string::npos;
                 found = name.find(noise)) {
                name.erase(found, noise.size());
            }
        }
        if (const std::size_t at_sign = name.find(" @ "); at_sign != std::string::npos) {
            name.erase(at_sign);  // the clock is not the name, and it is not the clock either
        }
        // Collapse the gaps the removals left.
        std::string tidy;
        bool spaced = true;
        for (const char c : name) {
            if (c == ' ') {
                if (!spaced) {
                    tidy += ' ';
                }
                spaced = true;
                continue;
            }
            tidy += c;
            spaced = false;
        }
        return std::string(trim(tidy));
    }
    return {};
}

}  // namespace crucible::util
