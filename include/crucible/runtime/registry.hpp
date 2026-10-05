// SPDX-License-Identifier: MIT
//
// What is installed in the runtimes directory, and getting it into ggml.
//
// Crucible keeps every runtime in one directory. Installing one puts a shared
// library there, uninstalling deletes it, and startup hands the directory to
// ggml. Nothing else in the program needs to know a backend can come and go.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "crucible/runtime/backend.hpp"

namespace crucible {

/// One runtime, as the settings screen sees it.
struct RuntimeStatus {
    BackendKind kind = BackendKind::Cpu;

    /// A module for this backend is present in the runtimes directory.
    bool installed = false;

    /// ggml loaded it this run and it reported at least one device. A runtime
    /// can be installed but inactive -- a CUDA build on a machine whose driver
    /// has since been removed still sits on disk, and saying so is more useful
    /// than pretending it works.
    bool   active       = false;
    int    device_count = 0;

    std::uintmax_t bytes = 0;   ///< total size of its module files

    /// Provenance, from the manifest. Empty for a runtime whose manifest entry
    /// was lost, which is treated as "cannot tell" rather than as a problem.
    std::string llama_tag;
    std::string built_at;

    /// "downloaded" or "built here", from the manifest. Empty for a runtime
    /// installed before the manifest recorded it.
    std::string source;

    /// Built against a different llama.cpp than this binary.
    ///
    /// Runtimes outlive the Crucible that made them -- they survive an uninstall
    /// that keeps your data, and a reinstall from newer source can land on top
    /// of them. ggml's internal structures are not stable across releases, so
    /// such a module loads and then crashes on the first tensor. Saying so is
    /// the difference between "rebuild this one" and an unexplained crash.
    bool stale = false;

    /// The tag this binary needs, for the message that explains `stale`.
    static std::string_view required_llama_tag();

    /// Whether a build could be attempted right now, and what is missing if
    /// not -- "needs nvcc" is a far better answer than a failed build.
    bool        buildable = true;
    std::string blocker;

    /// What an installed runtime still needs before it can load, or empty.
    /// Only CUDA has anything here: NVIDIA's own libraries, which a machine
    /// with a driver and no toolkit does not have. See cuda_libraries.hpp.
    std::string missing;

    /// The module files, so uninstall knows what to delete.
    std::vector<std::filesystem::path> files;

    /// The one of them ggml will actually load, empty when there is no choice
    /// to make. The CPU backend installs fourteen modules -- one per x86-64
    /// feature level -- and runs exactly one of them; without this the other
    /// thirteen look like thirteen runtimes.
    std::filesystem::path preferred;

    /// "4.1 MB", for the list.
    std::string size_label() const;
};

/// The runtimes directory as Crucible sees it.
///
/// Cheap to construct and safe to re-scan; the settings screen builds one per
/// refresh rather than holding state that can go stale.
class RuntimeRegistry {
public:
    /// Hand the runtimes directory to ggml. Call once, before any model is
    /// loaded; ggml scores the candidates and registers the best of each kind.
    ///
    /// A fresh install has nothing here: Crucible ships no backends, and the
    /// settings screen is what fills this directory.
    static void load_all();

    /// Register a runtime that was installed while Crucible was already running,
    /// so a backend built from the settings screen can be used without a
    /// restart.
    ///
    /// Idempotent: a backend that already has devices registered is left
    /// alone, because ggml would otherwise register a second copy of every one
    /// of them. Returns false with `error` set when the module is there but
    /// will not load -- an unsupported GPU, or a driver that is not installed.
    static bool activate(BackendKind kind, std::string& error);

    /// True when at least one backend module is installed. False on a fresh
    /// install, which is the state where no model can be loaded at all.
    static bool any_installed();

    /// True when this binary can load runtimes at all. A monolithic build
    /// (-DCRUCIBLE_BACKEND_DL=OFF) cannot, and the settings screen says so
    /// instead of offering buttons that could not work.
    static bool loadable_backends_supported();

    /// Look at the runtimes directory and at what ggml actually registered.
    static std::vector<RuntimeStatus> scan();

    /// Delete a runtime's module files. Takes effect on the next start, since
    /// ggml cannot unload a backend that loaded models may still be using.
    static bool remove(BackendKind kind, std::string& error);

private:
    /// Recorded next to the modules so a runtime can say where it came from.
    static std::filesystem::path manifest_file();
};

}  // namespace crucible
