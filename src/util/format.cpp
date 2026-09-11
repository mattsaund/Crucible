// SPDX-License-Identifier: MIT
#include "crucible/util/format.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace crucible::format {

std::string trim(std::string text) {
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

std::string number(double value, int precision) {
    std::array<char, 64> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%.*f", precision, value);
    return buffer.data();
}

std::string duration_ms(long milliseconds) {
    if (milliseconds < 1000) {
        return std::to_string(milliseconds) + "ms";
    }
    return number(static_cast<double>(milliseconds) / 1000.0, 1) + "s";
}

std::string bytes(std::uintmax_t count) {
    static constexpr std::array<const char*, 5> kUnits{{"B", "KB", "MB", "GB", "TB"}};

    auto        value = static_cast<double>(count);
    std::size_t unit  = 0;
    while (value >= 1024.0 && unit + 1 < kUnits.size()) {
        value /= 1024.0;
        ++unit;
    }

    std::array<char, 32> buffer{};
    // Whole bytes need no decimal point; anything larger reads better with one.
    std::snprintf(buffer.data(), buffer.size(), unit == 0 ? "%.0f %s" : "%.1f %s",
                  value, kUnits[unit]);
    return buffer.data();
}

}  // namespace crucible::format

namespace crucible::format {

std::string short_path(const std::filesystem::path& path) {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return path.string();
    }
    const std::string text(path.string());
    const std::string prefix(home);
    if (text.rfind(prefix, 0) == 0 && text.size() > prefix.size()) {
        return "~" + text.substr(prefix.size());
    }
    return text;
}

}  // namespace crucible::format
