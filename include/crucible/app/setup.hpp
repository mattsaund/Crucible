// SPDX-License-Identifier: MIT
//
// Everything Crucible needs that its download does not carry, fetched when it
// starts.
//
// A disk image, a setup .exe and an AppImage carry the program and nothing
// else, and for a long time everything else was a visit to the settings
// screen: a runtime to run a model on, a training environment, and before any
// of those a Python the machine might not have. Now the window does it, in the
// order things are needed:
//
//   1. Crucible's own Python, which routing and cooks run on (lab/python.hpp);
//   2. the compute runtimes this machine can use -- the CPU always, CUDA with
//      NVIDIA's libraries where there is an NVIDIA driver, Metal or Vulkan;
//   3. the training environment, built from that Python, which is also what
//      runs MLX models on a Mac.
//
// Only what is missing, and nothing at all on a machine that has it all. It
// drives the same runtime builder and training installer the settings screen
// does, so those pages show the same progress, and a failure is said and can
// be retried without the rest being undone.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crucible/lab/pyenv.hpp"
#include "crucible/runtime/backend.hpp"
#include "crucible/runtime/builder.hpp"

namespace crucible {

class Setup {
public:
    struct Item {
        enum class State { Waiting, Working, Done, Failed };

        std::string   id;              ///< "python", "runtime-cuda", "trainer"
        std::string   label;           ///< "Python 3.12.15", "CUDA runtime"
        State         state = State::Waiting;
        std::string   detail;          ///< what it is doing, or why it failed
        float         progress = -1.0F;  ///< 0..1 when it is measured, else negative
        std::uint64_t download = 0;    ///< roughly what it costs, when known
    };

    /// `on_change` is called from the setup's thread whenever something moved.
    /// `on_runtime` is called from it when a runtime has been installed, so
    /// the models can be put on the new hardware.
    Setup(RuntimeBuilder& runtimes, lab::pyenv::Installer& trainer,
          std::function<void()> on_change, std::function<void()> on_runtime);
    ~Setup();
    Setup(const Setup&)            = delete;
    Setup& operator=(const Setup&) = delete;

    /// Fetch whatever is missing, on a thread of its own. Nothing happens when
    /// nothing is, or one is already running. CRUCIBLE_NO_SETUP=1 turns it off,
    /// for a test harness that brings its own.
    void start();

    /// Stop waiting and return. An install already handed to the runtime
    /// builder or the training installer is theirs to stop.
    void stop();

    bool running() const;
    std::vector<Item> items() const;

private:
    void run();
    void update(std::size_t index, Item::State state, std::string detail, float progress);

    RuntimeBuilder&        runtimes_;
    lab::pyenv::Installer& trainer_;
    std::function<void()>  on_change_;
    std::function<void()>  on_runtime_;

    mutable std::mutex     mutex_;
    std::vector<Item>      items_;
    std::vector<BackendKind> kinds_;   ///< parallel to the runtime items, in order
    bool                   running_ = false;
    std::atomic<bool>      stop_{false};
    std::thread            worker_;
};

}  // namespace crucible
