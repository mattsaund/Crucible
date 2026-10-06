// SPDX-License-Identifier: MIT
//
// UTF-8 helpers.
//
// Small, but load-bearing in two places: a model token can end mid-codepoint,
// and a text cursor must not land inside one.
#include "crucible/util/text.hpp"

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace crucible::detail {

std::size_t utf8_prev(const std::string& text, std::size_t index) {
    if (index == 0) {
        return 0;
    }
    --index;
    while (index > 0 && (static_cast<unsigned char>(text[index]) & 0xC0U) == 0x80U) {
        --index;
    }
    return index;
}

std::size_t utf8_next(const std::string& text, std::size_t index) {
    if (index >= text.size()) {
        return text.size();
    }
    ++index;
    while (index < text.size() && (static_cast<unsigned char>(text[index]) & 0xC0U) == 0x80U) {
        ++index;
    }
    return index;
}

int utf8_length(unsigned char lead) {
    if ((lead & 0x80U) == 0x00U) { return 1; }
    if ((lead & 0xE0U) == 0xC0U) { return 2; }
    if ((lead & 0xF0U) == 0xE0U) { return 3; }
    if ((lead & 0xF8U) == 0xF0U) { return 4; }
    return 1;
}

std::string take_complete_utf8(std::string& buffer) {
    std::size_t cut = buffer.size();

    // A sequence is at most 4 bytes, so only the last 4 can be incomplete.
    const std::size_t limit = buffer.size() > 4 ? buffer.size() - 4 : 0;
    for (std::size_t i = buffer.size(); i-- > limit;) {
        const auto byte = static_cast<unsigned char>(buffer[i]);
        if ((byte & 0xC0U) == 0x80U) {
            continue;  // continuation byte: keep walking back to the lead
        }
        const auto needed = static_cast<std::size_t>(utf8_length(byte));
        if (i + needed > buffer.size()) {
            cut = i;  // this sequence has not arrived in full yet
        }
        break;
    }

    std::string complete = buffer.substr(0, cut);
    buffer.erase(0, cut);
    return complete;
}

namespace {

/// The length of the well-formed sequence at `text[i]`, or 0 when there is
/// none: a stray continuation byte, a truncated sequence, an overlong form, a
/// surrogate or a value past U+10FFFF.
std::size_t utf8_sequence(std::string_view text, std::size_t i) {
    const auto byte = [&text](std::size_t at) { return static_cast<unsigned char>(text[at]); };
    const unsigned char lead = byte(i);
    if (lead < 0x80) {
        return 1;
    }
    std::size_t   length = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        if (lead == 0xE0) { low = 0xA0; }
        if (lead == 0xED) { high = 0x9F; }   // no surrogates
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        if (lead == 0xF0) { low = 0x90; }
        if (lead == 0xF4) { high = 0x8F; }   // nothing past U+10FFFF
    } else {
        return 0;
    }
    if (i + length > text.size()) {
        return 0;
    }
    if (byte(i + 1) < low || byte(i + 1) > high) {
        return 0;
    }
    for (std::size_t k = 2; k < length; ++k) {
        if (byte(i + k) < 0x80 || byte(i + k) > 0xBF) {
            return 0;
        }
    }
    return length;
}

}  // namespace

bool is_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t length = utf8_sequence(text, i);
        if (length == 0) {
            return false;
        }
        i += length;
    }
    return true;
}

std::string scrub_utf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t length = utf8_sequence(text, i);
        if (length == 0) {
            out += "\xEF\xBF\xBD";   // U+FFFD
            ++i;
        } else {
            out.append(text.substr(i, length));
            i += length;
        }
    }
    return out;
}

std::string console_to_utf8(std::string_view text) {
    if (is_utf8(text)) {
        return std::string(text);
    }
#if defined(_WIN32)
    const UINT page = ::GetConsoleOutputCP() != 0 ? ::GetConsoleOutputCP() : ::GetOEMCP();
    const int wide = ::MultiByteToWideChar(page, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (wide > 0) {
        std::wstring buffer(static_cast<std::size_t>(wide), L'\0');
        ::MultiByteToWideChar(page, 0, text.data(), static_cast<int>(text.size()), buffer.data(), wide);
        const int narrow = ::WideCharToMultiByte(CP_UTF8, 0, buffer.data(), wide, nullptr, 0, nullptr, nullptr);
        if (narrow > 0) {
            std::string out(static_cast<std::size_t>(narrow), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, buffer.data(), wide, out.data(), narrow, nullptr, nullptr);
            return scrub_utf8(out);
        }
    }
#endif
    return scrub_utf8(text);
}

}  // namespace crucible::detail

