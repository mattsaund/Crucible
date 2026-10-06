// SPDX-License-Identifier: MIT
//
// See crash.hpp.
#include "crash.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <system_error>

#include "crucible/util/platform.hpp"

#if defined(_WIN32)
#  include <windows.h>
#  include <dbghelp.h>
#else
#  include <execinfo.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <unistd.h>
#endif

#ifndef CRUCIBLE_VERSION
#define CRUCIBLE_VERSION "0.0.0"
#endif

namespace crucible::gui::crash {
namespace {

std::filesystem::path seen_marker(const std::filesystem::path& log) {
    return log.string() + ".seen";
}

/// The heading of one report: when, and which build.
std::string heading(const char* what) {
    const std::tm local = util::local_time(std::time(nullptr));
    char when[32];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &local);
    return std::string("\n=== ") + when + "  Crucible " + CRUCIBLE_VERSION + "  " + what + "\n";
}

#if defined(_WIN32)

std::FILE* g_log = nullptr;

LONG WINAPI report(EXCEPTION_POINTERS* info) {
    if (g_log == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    char what[64];
    std::snprintf(what, sizeof(what), "exception 0x%08lX",
                  static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode));
    std::fputs(heading(what).c_str(), g_log);

    HANDLE process = ::GetCurrentProcess();
    ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    const bool symbols = ::SymInitialize(process, nullptr, TRUE) != FALSE;

    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame{};
#if defined(_M_X64) || defined(__x86_64__)
    const DWORD machine    = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset    = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
#elif defined(_M_ARM64) || defined(__aarch64__)
    const DWORD machine    = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset    = context.Pc;
    frame.AddrFrame.Offset = context.Fp;
    frame.AddrStack.Offset = context.Sp;
#else
    const DWORD machine    = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset    = context.Eip;
    frame.AddrFrame.Offset = context.Ebp;
    frame.AddrStack.Offset = context.Esp;
#endif
    frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    for (int depth = 0; depth < 40; ++depth) {
        if (::StackWalk64(machine, process, ::GetCurrentThread(), &frame, &context, nullptr,
                          ::SymFunctionTableAccess64, ::SymGetModuleBase64, nullptr) == FALSE
            || frame.AddrPC.Offset == 0) {
            break;
        }
        const DWORD64 address = frame.AddrPC.Offset;
        char    module_path[MAX_PATH] = "?";
        HMODULE module                = nullptr;
        if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                     | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCSTR>(static_cast<std::uintptr_t>(address)),
                                 &module) != FALSE) {
            ::GetModuleFileNameA(module, module_path, MAX_PATH);
        }
        const char* slash       = std::strrchr(module_path, '\\');
        const char* module_name = slash != nullptr ? slash + 1 : module_path;

        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256] = {};
        auto* symbol         = reinterpret_cast<SYMBOL_INFO*>(storage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen   = 255;
        DWORD64 displacement = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct       = sizeof(line);
        DWORD line_displacement = 0;
        if (symbols && ::SymFromAddr(process, address, &displacement, symbol) != FALSE) {
            if (::SymGetLineFromAddr64(process, address, &line_displacement, &line) != FALSE) {
                std::fprintf(g_log, "  %s!%s  %s:%lu\n", module_name, symbol->Name, line.FileName,
                             static_cast<unsigned long>(line.LineNumber));
            } else {
                std::fprintf(g_log, "  %s!%s+0x%llx\n", module_name, symbol->Name,
                             static_cast<unsigned long long>(displacement));
            }
        } else {
            const auto base = static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(module));
            std::fprintf(g_log, "  %s+0x%llx\n", module_name,
                         static_cast<unsigned long long>(address) - base);
        }
    }
    std::fflush(g_log);
    return EXCEPTION_CONTINUE_SEARCH;
}

void write_line(const std::string& text) {
    if (g_log != nullptr) {
        std::fputs(text.c_str(), g_log);
        std::fflush(g_log);
    }
}

#else

int g_fd = -1;

void write_line(const std::string& text) {
    if (g_fd >= 0) {
        const ssize_t ignored = ::write(g_fd, text.data(), text.size());
        (void)ignored;
    }
}

/// Only what is safe in a signal handler: write(2), and backtrace into a
/// buffer that already exists. The heading is built before, at install.
void report(int signal) {
    if (g_fd >= 0) {
        char line[96];
        const int length = std::snprintf(line, sizeof(line), "\n=== signal %d  Crucible %s\n",
                                         signal, CRUCIBLE_VERSION);
        if (length > 0) {
            const ssize_t ignored = ::write(g_fd, line, static_cast<std::size_t>(length));
            (void)ignored;
        }
        void* frames[64];
        const int depth = ::backtrace(frames, 64);
        ::backtrace_symbols_fd(frames, depth, g_fd);
    }
    // And then die the way it was going to: the default action, re-raised.
    ::signal(signal, SIG_DFL);
    ::raise(signal);
}

#endif

/// An exception nobody caught: what it said, then the abort that follows
/// records where.
[[noreturn]] void on_terminate() {
    std::string said = "terminate";
    if (const std::exception_ptr pending = std::current_exception()) {
        try {
            std::rethrow_exception(pending);
        } catch (const std::exception& e) {
            said = std::string("uncaught exception: ") + e.what();
        } catch (...) {
            said = "uncaught exception of an unknown type";
        }
    }
    write_line(heading(said.c_str()));
    std::abort();
}

}  // namespace

void install(const std::filesystem::path& log) {
    std::error_code ec;
    std::filesystem::create_directories(log.parent_path(), ec);
#if defined(_WIN32)
    g_log = ::_wfopen(log.wstring().c_str(), L"a");
    // Room for the report to run in even when what ran out was the stack,
    // which a 1 MB Windows stack meets long before an 8 MB Linux one does.
    ULONG reserve = 64 * 1024;
    ::SetThreadStackGuarantee(&reserve);
    ::SetUnhandledExceptionFilter(report);
#else
    g_fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    // backtrace loads libgcc the first time it is called, which allocates:
    // done now, while that is safe, rather than first in a signal handler.
    void* warm[1];
    ::backtrace(warm, 1);
    for (const int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) {
        ::signal(signal, report);
    }
#endif
    std::set_terminate(on_terminate);
}

std::string since_last_start(const std::filesystem::path& log) {
    std::error_code ec;
    const auto crashed = std::filesystem::last_write_time(log, ec);
    if (ec || std::filesystem::file_size(log, ec) == 0 || ec) {
        return {};
    }
    const std::filesystem::path seen = seen_marker(log);
    std::error_code seen_ec;
    const auto noticed = std::filesystem::last_write_time(seen, seen_ec);
    if (!seen_ec && noticed >= crashed) {
        return {};
    }
    std::ofstream(seen) << "seen\n";
    return "Crucible closed unexpectedly last time -- what it was doing is in " + log.string();
}

}  // namespace crucible::gui::crash
