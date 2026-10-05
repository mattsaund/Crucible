// SPDX-License-Identifier: MIT
#include "crucible/app/setup_runtimes.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

#include "crucible/config/paths.hpp"
#include "crucible/runtime/backend.hpp"
#include "crucible/runtime/builder.hpp"
#include "crucible/runtime/registry.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/platform.hpp"
#include "crucible/util/subprocess.hpp"

namespace crucible {
namespace {

/// Overwrite one line, or print a new one where nobody is watching. The same
/// reasoning as the trainer's: a log full of eight hundred progress lines is
/// not a log, and a terminal showing only the first one is not progress.
class Line {
public:
    explicit Line(bool interactive) : interactive_(interactive) {}

    void show(const std::string& text) {
        if (text == last_) {
            return;
        }
        last_ = text;
        std::printf(interactive_ ? "\r\033[2K    %s" : "    %s\n", text.c_str());
        std::fflush(stdout);
    }

    void done() const {
        if (interactive_) {
            std::printf("\r\033[2K");
            std::fflush(stdout);
        }
    }

private:
    bool        interactive_;
    std::string last_;
};

/// Which backends this machine should actually end up with.
///
/// `backend_available_here` answers a platform question -- CUDA is not a thing
/// on a Mac -- and that is not the same as whether the hardware is there. A
/// CUDA module on a machine with no NVIDIA driver installs perfectly and then
/// reports no devices, which is a confusing thing to have been given by an
/// installer.
std::vector<BackendKind> wanted_here() {
    std::vector<BackendKind> wanted{BackendKind::Cpu};   // always, it is the floor

    for (const BackendInfo& info : all_backends()) {
        if (info.kind == BackendKind::Cpu || !backend_available_here(info.kind)) {
            continue;
        }
        if (info.kind == BackendKind::Cuda && !util::on_path("nvidia-smi")) {
            // The driver ships nvidia-smi and nothing else does, so its
            // absence is the cheapest honest answer to "is there an NVIDIA
            // card this can use".
            continue;
        }
        // Metal where the platform has it, and Vulkan everywhere else it is
        // offered: it is the backend that covers AMD and Intel, and it is a
        // second way to reach an NVIDIA card when a driver update breaks the
        // first.
        wanted.push_back(info.kind);
    }
    return wanted;
}

std::string name_of(BackendKind kind) {
    return std::string(backend_info(kind).id);
}

}  // namespace

int run_runtime_status() {
    // Modules have to be registered with ggml before it can say what devices
    // they found; without this every runtime reads as installed-but-inactive,
    // which is exactly what a working machine should not be told.
    RuntimeRegistry::load_all();
    const std::vector<RuntimeStatus> installed = RuntimeRegistry::scan();
    bool                             any_active = false;

    std::cout << "runtimes in " << paths::runtimes_dir().string() << "\n";
    for (const RuntimeStatus& status : installed) {
        if (!status.installed) {
            continue;
        }
        any_active = any_active || status.active;
        std::cout << "  " << name_of(status.kind)
                  << (status.active ? "  active" : "  installed, no devices")
                  << "  " << status.device_count << " device"
                  << (status.device_count == 1 ? "" : "s")
                  << "  " << format::bytes(status.bytes);
        if (status.stale) {
            std::cout << "  (built against another llama.cpp -- reinstall it)";
        }
        std::cout << "\n";
    }
    if (!any_active) {
        std::cout << "  nothing active. Install with: crucible --install-runtimes\n";
    }
    return any_active ? 0 : 1;
}

int run_runtime_setup(bool quiet, bool force) {
    RuntimeRegistry::load_all();
    const std::vector<BackendKind>   wanted    = wanted_here();
    const std::vector<RuntimeStatus> installed = RuntimeRegistry::scan();

    const auto already = [&installed](BackendKind kind) {
        for (const RuntimeStatus& status : installed) {
            if (status.kind == kind && status.installed && !status.stale
                && status.missing.empty()) {
                return true;
            }
        }
        return false;
    };

    std::vector<BackendKind> todo;
    for (const BackendKind kind : wanted) {
        if (force || !already(kind)) {
            todo.push_back(kind);
        }
    }
    if (todo.empty()) {
        if (!quiet) {
            std::cout << "  Compute runtimes are already installed.\n";
        }
        return 2;
    }

    if (!quiet) {
        std::cout << "  Installing compute runtimes:";
        for (const BackendKind kind : todo) {
            std::cout << " " << name_of(kind);
        }
        std::cout << "\n  Downloaded where a build is published for this platform.\n";
    }

    const bool interactive = util::stdin_is_a_terminal() && !quiet;
    int        failed      = 0;

    for (const BackendKind kind : todo) {
        Line           line(interactive);
        RuntimeBuilder builder;
        builder.start(kind, {});

        for (;;) {
            const BuildProgress progress = builder.progress();
            if (!quiet) {
                line.show(name_of(kind) + ": " + progress.label());
            }
            if (progress.finished()) {
                line.done();
                if (progress.phase == BuildProgress::Phase::Done) {
                    if (!quiet) {
                        std::cout << "    " << name_of(kind) << " ready\n";
                    }
                } else {
                    // Not fatal, and not silent either. A machine with no CUDA
                    // toolkit and no published module for its platform is a
                    // machine that runs on the processor, which works.
                    ++failed;
                    std::cerr << "    " << name_of(kind) << " was not installed: "
                              << progress.error << "\n";
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

    if (!quiet) {
        run_runtime_status();
    }
    return failed == 0 ? 0 : 1;
}

}  // namespace crucible
