// SPDX-License-Identifier: MIT
//
// crucible and crucible-engine: which runtime, and nothing else.
//
// Crucible is built as a runtime: the program as a loadable module, with the
// llama.cpp it was built with beside it. This is the whole of what bin/ holds,
// and all it does is pick the runtime and hand over. So a newer llama.cpp is
// a runtime to download and choose, as in LM Studio, and not a new Crucible.
//
// The choice, in order:
//   1. CRUCIBLE_RUNTIME_DIR, a folder holding the module: for trying one.
//   2. runtimes/active in the data folder: the llama.cpp tag chosen in
//      Settings, Runtimes, whose runtime for this Crucible is in
//      runtimes/core/<version>/<tag>/. One built for another Crucible is
//      passed over: an update brings its own.
//   3. The runtime the installer put beside this launcher, which is always
//      there: in lib/crucible/ on Linux and macOS, beside it on Windows.
// One that will not load is passed over for the next, and the program is told
// why in CRUCIBLE_RUNTIME_PASSED_OVER, to say so.
//
// Deliberately no ggml and no llama.cpp in here: one process holds one, and
// it is the runtime's.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "crucible/config/paths.hpp"

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <signal.h>
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <mach-o/dyld.h>
#  endif
#endif

namespace {

using Entry = int (*)(int, char**);

/// The folder this launcher is in.
std::filesystem::path own_dir() {
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD  length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    std::error_code ec;
    const std::filesystem::path real = std::filesystem::canonical(buffer.c_str(), ec);
    return (ec ? std::filesystem::path(buffer.c_str()) : real).parent_path();
#else
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}

/// Load the module at `file` and find its entry. Empty `why` when it did.
Entry open(const std::filesystem::path& file, std::string& why) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        why = file.string() + " is not there";
        return nullptr;
    }
#if defined(_WIN32)
    // Its own folder first for the libraries it needs: llama.cpp's, beside it.
    HMODULE module = LoadLibraryExW(file.wstring().c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module == nullptr) {
        why = file.string() + " would not load (error " + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    auto entry = reinterpret_cast<Entry>(GetProcAddress(module, "crucible_entry"));
#else
    void* module = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (module == nullptr) {
        const char* said = dlerror();
        why = said != nullptr ? said : file.string() + " would not load";
        return nullptr;
    }
    auto entry = reinterpret_cast<Entry>(dlsym(module, "crucible_entry"));
#endif
    if (entry == nullptr) {
        why = file.string() + " is not a Crucible runtime";
    }
    return entry;
}

/// The first line of `file`, trimmed, or empty.
std::string first_line(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::string   line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\n')) {
        line.pop_back();
    }
    return line;
}

/// Tell the program something about where it was loaded from.
void say(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

/// A Crucible starting itself again on another runtime names itself in
/// CRUCIBLE_AFTER: wait for it to be gone, at most fifteen seconds, so two
/// never hold a model in the same card's memory.
void wait_for_the_one_before() {
    const char* named = std::getenv("CRUCIBLE_AFTER");
    if (named == nullptr || *named == '\0') {
        return;
    }
    const unsigned long pid = std::strtoul(named, nullptr, 10);
    say("CRUCIBLE_AFTER", "");
    if (pid == 0) {
        return;
    }
#if defined(_WIN32)
    if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid)); process != nullptr) {
        WaitForSingleObject(process, 15000);
        CloseHandle(process);
    }
#else
    for (int tenths = 0; tenths < 150 && kill(static_cast<pid_t>(pid), 0) == 0; ++tenths) {
        usleep(100000);
    }
#endif
}

}  // namespace

int main(int argc, char** argv) {
    wait_for_the_one_before();
    // What the Crucible that started this one was told is not true of this
    // one: a runtime it passed over may be the one chosen now.
    for (const char* told : {"CRUCIBLE_RUNTIME_PASSED_OVER", "CRUCIBLE_RUNTIME_FILE", "CRUCIBLE_BUNDLED_RUNTIME"}) {
        say(told, "");
    }
    const std::filesystem::path here = own_dir();
    std::vector<std::filesystem::path> candidates;
    if (const char* dir = std::getenv("CRUCIBLE_RUNTIME_DIR"); dir != nullptr && *dir != '\0') {
        candidates.push_back(std::filesystem::path(dir) / CRUCIBLE_MODULE_FILE);
    }
    const std::filesystem::path runtimes = crucible::paths::runtimes_dir();
    if (const std::string tag = first_line(runtimes / "active");
        !tag.empty() && tag.find_first_of("/\\.") == std::string::npos) {
        candidates.push_back(runtimes / "core" / CRUCIBLE_VERSION / tag / CRUCIBLE_MODULE_FILE);
    }
    candidates.push_back(here / CRUCIBLE_MODULE_FILE);
    candidates.push_back(here / ".." / "lib" / "crucible" / CRUCIBLE_MODULE_FILE);
    // Where the installer's runtime is, which a choice can always go back to.
    for (const std::filesystem::path& bundled : {here, here / ".." / "lib" / "crucible"}) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(bundled / CRUCIBLE_MODULE_FILE, ec)) {
            say("CRUCIBLE_BUNDLED_RUNTIME", bundled.lexically_normal().string());
            break;
        }
    }

    std::string passed_over;
    for (const std::filesystem::path& file : candidates) {
        std::string why;
        if (const Entry entry = open(file, why)) {
            if (!passed_over.empty()) {
                say("CRUCIBLE_RUNTIME_PASSED_OVER", passed_over);
            }
            say("CRUCIBLE_RUNTIME_FILE", file.lexically_normal().string());
            return entry(argc, argv);
        }
        // A runtime that is simply not there is not worth reporting: most
        // machines have no chosen one, and only one of the bundled places
        // is the right one for this layout.
        std::error_code ec;
        if (std::filesystem::exists(file, ec)) {
            passed_over += (passed_over.empty() ? "" : "; ") + why;
        }
    }
    std::fprintf(stderr, "crucible: no runtime would load%s%s\n", passed_over.empty() ? "" : ": ",
                 passed_over.c_str());
    return 1;
}
