// SPDX-License-Identifier: MIT
//
// Building a GPU runtime from source, on demand, from inside the TUI.
//
// Installing CUDA support means compiling one ggml backend against the same
// llama.cpp the binary was built from -- a few minutes of cmake that must not
// block the UI and must be abandonable halfway through. This runs it on a
// worker thread and publishes progress the settings screen polls.
//
// It deliberately does *not* install system packages. That needs root, and a
// TUI is the wrong place to ask for it -- so a missing SDK is reported as
// advice, with the exact command for the package manager this machine has
// (see install_hint in runtime/backend.hpp), and the build is refused before
// it starts rather than failed several minutes in.
//
// The installer does not put the SDKs in place either. It used to -- a Vulkan
// SDK on every machine and an offer of several gigabytes of CUDA -- against
// the possibility that a runtime would be built later. Which backend to build,
// and therefore which SDK is wanted, is a question that can only be answered
// on the machine at the moment it is asked, and this is where it is asked.
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crucible/runtime/backend.hpp"
#include "crucible/util/subprocess.hpp"

namespace crucible {

/// Which CUDA architectures a build should target, given the compute
/// capabilities `present` in the machine and the ones `targetable` by the
/// installed nvcc. Both are written the way CMake writes them: 8.9 is 89.
///
/// Real code for every card that is there, and PTX for one architecture on top
/// -- the newest the compiler knows, so a card too new for the toolkit (a
/// Blackwell card against CUDA 12.0, say) is compiled by the driver at first
/// use, and so is a card added after the build. Returns an empty string when
/// either list is empty, and the build then falls back to llama.cpp's defaults.
///
/// Left to itself llama.cpp targets eight architectures, five of them for cards
/// going back to Maxwell. Each is a full pass of nvcc over 142 files: on a
/// six-core machine with an Ampere, an Ada and a Blackwell card that is sixteen
/// minutes and a 527 MB module. Targeting the three that matter is ten minutes
/// and 309 MB.
std::string cuda_architectures(const std::vector<int>& present,
                               const std::vector<int>& targetable);


/// Where a build has got to. Copied under a lock for each frame.
struct BuildProgress {
    enum class Phase {
        Idle,
        FetchingSource,  ///< cloning llama.cpp, the first time only
        Configuring,     ///< cmake -S -B: finds the SDK, generates the build
        Compiling,       ///< the long one, and the only phase with a percentage
        Installing,      ///< copying the module into the runtimes directory
        Done,
        Failed,
        Cancelled,
    };

    Phase       phase   = Phase::Idle;
    BackendKind kind    = BackendKind::Cpu;
    float       percent = 0.0F;   ///< 0..1, meaningful during Compiling
    std::string step;             ///< the file being compiled, or the phase
    std::string error;            ///< set when phase is Failed
    std::chrono::steady_clock::time_point started{};

    /// The tail of the build log, for a failure the user has to act on.
    std::vector<std::string> log_tail;

    /// Where the full log lives, which is what a bug report should carry.
    std::filesystem::path log_file;

    bool finished() const {
        return phase == Phase::Done || phase == Phase::Failed || phase == Phase::Cancelled;
    }
    bool running() const { return phase != Phase::Idle && !finished(); }

    /// "configuring", "compiling 42%" -- one line for the settings screen.
    std::string label() const;
};

/// Builds one runtime at a time, on a thread of its own.
class RuntimeBuilder {
public:
    RuntimeBuilder() = default;
    ~RuntimeBuilder();
    RuntimeBuilder(const RuntimeBuilder&)            = delete;
    RuntimeBuilder& operator=(const RuntimeBuilder&) = delete;

    /// Start building `kind`. `on_change` is called from the worker whenever
    /// progress moved, so the UI can redraw; it must be safe off the UI thread.
    /// Returns false if a build is already running.
    bool start(BackendKind kind, std::function<void()> on_change);

    /// Ask the running build to stop. The compiler is signalled, so this takes
    /// effect within a second rather than at the end of the build.
    void cancel();

    /// Cancel and wait for the worker to actually be gone.
    ///
    /// Needed at shutdown: the worker calls `on_change` to redraw, and that
    /// callback reaches into the application. Letting the thread outlive the
    /// objects it pokes is a use-after-free, so teardown blocks here.
    void stop();

    /// Forget a finished build, returning the builder to Idle.
    void dismiss();

    BuildProgress progress() const;

private:
    void run(BackendKind kind);

    /// Make sure runtime-src holds llama.cpp at the tag this binary was built
    /// against. Cloning it is the one step that needs the network.
    bool ensure_source(std::string& error);

    /// Run one command, streaming its output into the log and the progress.
    /// `parse_percent` turns "[ 42%]" lines into a percentage.
    bool run_command(const std::vector<std::string>& argv,
                     const std::filesystem::path& cwd,
                     BuildProgress::Phase phase,
                     bool parse_percent);

    void set_phase(BuildProgress::Phase phase, std::string step = {});
    void fail(std::string error);

    mutable std::mutex    mutex_;
    BuildProgress         progress_;
    std::vector<std::string> log_;      ///< the whole log, trimmed to a bound
    std::thread           worker_;
    std::atomic<bool>     cancel_{false};
    std::atomic<bool>     running_{false};
    std::function<void()> on_change_;

    /// The child currently running, so cancel() can signal it from the UI
    /// thread while the worker is blocked reading its output.
    std::mutex                         child_mutex_;
    std::unique_ptr<util::Subprocess>  child_;
};

}  // namespace crucible
