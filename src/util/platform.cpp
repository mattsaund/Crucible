// SPDX-License-Identifier: MIT
#include "crucible/util/platform.hpp"
#include "crucible/util/subprocess.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>
#  include <shellapi.h>
#else
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <mach-o/dyld.h>
#  endif
#endif

namespace crucible::util {

bool stdin_is_a_terminal() {
#if defined(_WIN32)
    return ::_isatty(::_fileno(stdin)) != 0;
#else
    return ::isatty(STDIN_FILENO) != 0;
#endif
}

void use_utf8_console() {
#if defined(_WIN32)
    // Output only. The input code page belongs to whoever is reading keys, and
    // changing it from under a console already doing so would be rude.
    ::SetConsoleOutputCP(CP_UTF8);
#endif
}

std::filesystem::path executable_path() {
#if defined(_WIN32)
    // GetModuleFileNameW truncates rather than failing, and says so only by
    // filling the buffer exactly, so the loop grows until it does not.
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD written =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            return std::filesystem::path(std::wstring(buffer.data(), written));
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    // _NSGetExecutablePath reports the size it wanted when the buffer is too
    // small, so this asks twice rather than guessing.
    std::uint32_t size = 0;
    ::_NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (::_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    // The path it gives may contain symlinks and `..`; the caller wants the
    // real file, because it is about to be deleted.
    std::error_code ec;
    const std::filesystem::path self(buffer.data());
    std::filesystem::path resolved = std::filesystem::canonical(self, ec);
    return ec ? self : resolved;
#else
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : self;
#endif
}

std::tm local_time(std::time_t when) {
    std::tm parts{};
#if defined(_WIN32)
    ::localtime_s(&parts, &when);
#else
    ::localtime_r(&when, &parts);
#endif
    return parts;
}

std::tm utc_time(std::time_t when) {
    std::tm parts{};
#if defined(_WIN32)
    ::gmtime_s(&parts, &when);
#else
    ::gmtime_r(&when, &parts);
#endif
    return parts;
}

void hide_folder(const std::filesystem::path& folder) {
#if defined(_WIN32)
    const DWORD attributes = ::GetFileAttributesW(folder.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN) == 0) {
        ::SetFileAttributesW(folder.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN);
    }
#else
    (void)folder;
#endif
}

std::vector<std::string> shell_command(const std::string& command) {
#if defined(_WIN32)
    // cmd rather than PowerShell: it is always present, it starts in
    // milliseconds, and `/c` takes the rest of the line as one command the way
    // `sh -c` does.
    return {"cmd", "/c", command};
#else
    return {"/bin/sh", "-c", command};
#endif
}

bool write_file_atomically(const std::filesystem::path& file, std::string_view text, std::string& error) {
    const std::filesystem::path temp = file.string() + ".tmp";
    std::error_code ignored;
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "could not write " + temp.string();
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
            std::filesystem::remove(temp, ignored);
            error = "could not write " + temp.string() + " (is the disk full?)";
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        std::filesystem::remove(temp, ignored);
        error = "could not replace " + file.string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool open_in_browser(const std::string& url, std::string& error) {
    if (url.rfind("https://", 0) != 0 || url.find_first_of("\"\n\r") != std::string::npos) {
        error = "only an https address is opened";
        return false;
    }
#if defined(_WIN32)
    const int wide = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    std::wstring address(static_cast<std::size_t>(wide > 0 ? wide : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, address.data(), wide);
    const bool opened = reinterpret_cast<INT_PTR>(
        ::ShellExecuteW(nullptr, L"open", address.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#elif defined(__APPLE__)
    const bool opened = start_detached({"open", url}, error);
#else
    const bool opened = start_detached({"xdg-open", url}, error);
#endif
    if (!opened && error.empty()) {
        error = "could not open a browser";
    }
    return opened;
}

}  // namespace crucible::util
