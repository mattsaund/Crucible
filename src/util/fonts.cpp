// SPDX-License-Identifier: MIT
//
// See fonts.hpp.
#include "crucible/util/fonts.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__APPLE__)
#  include <CoreFoundation/CoreFoundation.h>
#  include <CoreText/CoreText.h>
#else
#  include "crucible/util/subprocess.hpp"
#endif

namespace crucible::util {
namespace {

#if defined(_WIN32)
std::string utf8(const wchar_t* wide) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), bytes, nullptr, nullptr);
    out.pop_back();   // the terminator WideCharToMultiByte counted
    return out;
}

int CALLBACK collect(const LOGFONTW* font, const TEXTMETRICW* /*metrics*/, DWORD /*type*/,
                     LPARAM into) {
    // "@" is the same family turned on its side for vertical text.
    if (font != nullptr && font->lfFaceName[0] != L'@') {
        reinterpret_cast<std::set<std::string>*>(into)->insert(utf8(font->lfFaceName));
    }
    return 1;
}
#endif

}  // namespace

std::vector<std::string> installed_font_families() {
    std::set<std::string> families;
#if defined(_WIN32)
    HDC screen = GetDC(nullptr);
    if (screen != nullptr) {
        LOGFONTW query{};
        query.lfCharSet = DEFAULT_CHARSET;   // every script, so every family
        EnumFontFamiliesExW(screen, &query, collect, reinterpret_cast<LPARAM>(&families), 0);
        ReleaseDC(nullptr, screen);
    }
#elif defined(__APPLE__)
    if (CFArrayRef names = CTFontManagerCopyAvailableFontFamilyNames(); names != nullptr) {
        const CFIndex count = CFArrayGetCount(names);
        for (CFIndex i = 0; i < count; ++i) {
            const auto name = static_cast<CFStringRef>(CFArrayGetValueAtIndex(names, i));
            const CFIndex room = CFStringGetMaximumSizeForEncoding(CFStringGetLength(name),
                                                                   kCFStringEncodingUTF8) + 1;
            std::string text(static_cast<std::size_t>(room), '\0');
            if (CFStringGetCString(name, text.data(), room, kCFStringEncodingUTF8)) {
                text.resize(std::char_traits<char>::length(text.c_str()));
                families.insert(text);
            }
        }
        CFRelease(names);
    }
#else
    // The first name of each family: fontconfig lists translations after it,
    // and the first is the one a stylesheet is matched against.
    Subprocess child;
    std::string error;
    if (child.start({"fc-list", "--format", "%{family[0]}\n"}, {}, {}, error)) {
        std::string line;
        while (child.read_line(line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            families.insert(line);
        }
        child.wait();
    }
#endif
    std::vector<std::string> out;
    out.reserve(families.size());
    for (const std::string& family : families) {
        // A Mac keeps its own interface faces under names that start with a
        // dot, and they are not for asking for by name.
        if (!family.empty() && family.front() != '.') {
            out.push_back(family);
        }
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
            [](char x, char y) { return std::tolower(static_cast<unsigned char>(x))
                                      < std::tolower(static_cast<unsigned char>(y)); });
    });
    return out;
}

}  // namespace crucible::util
