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

std::string to_lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
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

std::string base64(std::string_view bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const std::size_t size = bytes.size();
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3) {
        const unsigned int a = static_cast<unsigned char>(bytes[i]);
        const unsigned int b = i + 1 < size ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const unsigned int c = i + 2 < size ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        const unsigned int triple = (a << 16) | (b << 8) | c;
        out += kAlphabet[(triple >> 18) & 0x3F];
        out += kAlphabet[(triple >> 12) & 0x3F];
        out += i + 1 < size ? kAlphabet[(triple >> 6) & 0x3F] : '=';
        out += i + 2 < size ? kAlphabet[triple & 0x3F] : '=';
    }
    return out;
}

bool from_base64(std::string_view text, std::string& out) {
    out.clear();
    out.reserve(text.size() / 4 * 3);
    unsigned int buffer = 0;
    int          bits   = 0;
    for (const char c : text) {
        int value = -1;
        if (c >= 'A' && c <= 'Z')      { value = c - 'A'; }
        else if (c >= 'a' && c <= 'z') { value = c - 'a' + 26; }
        else if (c >= '0' && c <= '9') { value = c - '0' + 52; }
        else if (c == '+')             { value = 62; }
        else if (c == '/')             { value = 63; }
        else if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t') { continue; }
        else { return false; }
        buffer = (buffer << 6) | static_cast<unsigned int>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buffer >> bits) & 0xFF);
        }
    }
    return true;
}

}  // namespace crucible::format
