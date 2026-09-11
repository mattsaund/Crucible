// SPDX-License-Identifier: MIT
//
// The entry point. Every case in the other test files registers itself at
// static-init time, so this does not need to know any of them by name.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>

#if defined(_WIN32)
#  include <windows.h>
#  include <dbghelp.h>
#endif

#include "harness.hpp"

#if defined(_WIN32)
namespace {

/// Where a crash happened, printed before the process goes.
///
/// On Windows a crash reaches the CTest log as one line -- "Exception:
/// SegFault" -- with no stack and no test name, and a CI runner has no
/// debugger to ask. This says which case was running, what the fault was, and
/// the calls that led to it: by name and line where the PDB files allow, and
/// as module+offset where they do not.
LONG WINAPI report_crash(EXCEPTION_POINTERS* info) {
    std::fprintf(stderr, "\n    CRASH in %s: exception 0x%08lX\n",
                 harness::current_test().c_str(),
                 static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode));

    HANDLE process = ::GetCurrentProcess();
    ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    const bool symbols = ::SymInitialize(process, nullptr, TRUE) != FALSE;

    // The stack of the faulting code, not of this handler.
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

    for (int depth = 0; depth < 32; ++depth) {
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
        line.SizeOfStruct      = sizeof(line);
        DWORD line_displacement = 0;

        if (symbols && ::SymFromAddr(process, address, &displacement, symbol) != FALSE) {
            if (::SymGetLineFromAddr64(process, address, &line_displacement, &line) != FALSE) {
                std::fprintf(stderr, "      %s!%s  %s:%lu\n", module_name, symbol->Name,
                             line.FileName, static_cast<unsigned long>(line.LineNumber));
            } else {
                std::fprintf(stderr, "      %s!%s+0x%llx\n", module_name, symbol->Name,
                             static_cast<unsigned long long>(displacement));
            }
        } else {
            const auto base = static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(module));
            std::fprintf(stderr, "      %s+0x%llx\n", module_name,
                         static_cast<unsigned long long>(address) - base);
        }
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace
#endif

int main() {
#if defined(_WIN32)
    // Room for the report to run in even when what ran out was the stack --
    // which a 1 MB Windows stack meets long before an 8 MB Linux one does.
    ULONG reserve = 64 * 1024;
    ::SetThreadStackGuarantee(&reserve);
    ::SetUnhandledExceptionFilter(report_crash);
#endif
    std::cout << "Crucible core tests\n\n";
    return harness::run_all();
}
