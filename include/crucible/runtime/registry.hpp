// SPDX-License-Identifier: MIT
//
// What is installed in the runtimes directory, and getting it into ggml.
//
// Crucible keeps every runtime in one directory. Installing one puts a shared
// library there, uninstalling deletes it, and startup loads what is there into
// ggml, one backend at a time. Nothing else in the program needs to know a
// backend can come and go.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "crucible/runtime/backend.hpp"

struct ggml_backend_device;   // ggml's own; a ggml_backend_dev_t points to one

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
/// Nothing but static functions: whatever they need of the directory they read
/// afresh, rather than holding state that can go stale.
class RuntimeRegistry {
public:
    /// Load every runtime in loadable() into ggml. Call once, before any model
    /// is loaded; each backend's modules are scored and the best of them is
    /// registered, the way activate() does it.
    ///
    /// A fresh install has nothing here: Crucible ships no backends. The
    /// installer, the window's setup when it starts, and the settings screen
    /// are what fill this directory.
    static void load_all();

    /// The backends load_all opens, in the order it opens them: every one
    /// installed, except a runtime built against another llama.cpp.
    ///
    /// That one stays closed. ggml turns it away, but a module can stay in
    /// memory once opened (on Linux CUDA and Vulkan always do), and opening
    /// the same path again hands back that copy. So the first start after an
    /// update opened the old modules, downloaded the new ones over them, and
    /// could not load those until Crucible was restarted.
    static std::vector<BackendKind> loadable();

    /// Register a runtime that was installed while Crucible was already running,
    /// so a backend built from the settings screen can be used without a
    /// restart.
    ///
    /// Idempotent: a backend that already has devices registered is left
    /// alone, because ggml would otherwise register a second copy of every one
    /// of them. Returns false with `error` set when the module is there but
    /// will not load -- an unsupported GPU, or a driver that is not installed.
    ///
    /// Waits for any model load under way, which reads the lists this
    /// changes from start to finish: see Reading.
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

    /// The devices ggml has registered, in its order, which is the order
    /// llama.cpp numbers them in. Read ggml's list here and nowhere else.
    ///
    /// ggml keeps it in a plain list that loading a backend appends to, and
    /// guards it with nothing: read on one thread while another activates a
    /// runtime, it can be read in the middle of being moved. So it is copied
    /// out under the lock a load takes, and the copy is what gets read. Each
    /// device in it stays valid for the life of the program, because a
    /// runtime is never unloaded.
    static std::vector<ggml_backend_device*> devices();

    /// ggml's lists held still for a call into llama.cpp that reads them.
    ///
    /// llama.cpp walks them itself: a model's load to choose its cards and
    /// find the processor, a context's creation, a projector's and a
    /// quantization to find the processor again. Each of those is made with
    /// one of these alive, and while any is, no runtime is loaded; one being
    /// loaded is finished before any opens. Any number can be open at once,
    /// so loads still go side by side, and only a runtime arriving waits for
    /// them, as they wait for it.
    ///
    /// Held for the whole of a load, dry runs and all, which is what
    /// covering llama.cpp's reads costs: a runtime installed during a load
    /// is registered when the load is over.
    ///
    /// One opened inside another on the same thread, and devices() inside
    /// either, take nothing more: the lock is not one a thread may take
    /// twice, even to read, since on a Mac and on Windows a second read
    /// waits behind a runtime that is waiting for the first. And a thread
    /// with one open is refused a runtime of its own (activate, load_all)
    /// rather than left waiting for itself.
    class Reading {
    public:
        Reading();
        ~Reading();
        Reading(const Reading&)            = delete;
        Reading& operator=(const Reading&) = delete;
    };

    /// llama_backend_init, made with ggml's lists to itself. With no runtime
    /// registered it loads every ggml module beside the program and in the
    /// working directory, which changes the lists as a runtime's load does.
    /// Before any model is loaded, and never inside a Reading.
    static void init_llama();

    /// Delete a runtime's module files. Takes effect on the next start, since
    /// ggml cannot unload a backend that loaded models may still be using.
    static bool remove(BackendKind kind, std::string& error);

private:
    /// Recorded next to the modules so a runtime can say where it came from.
    static std::filesystem::path manifest_file();
};

/// Put the module `from` at `to`, replacing what is there, even while that
/// is loaded: on POSIX a rename does it, and the running process keeps the
/// old file; on Windows a loaded DLL can be renamed but not replaced, so it
/// is moved aside to "<name>.old" first, and swept up on a later start.
bool place_module(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec);

/// Take the module `file` out of use: deleted, or on Windows, when it is
/// loaded, moved aside to "<name>.old" to be swept up later.
bool retire_module(const std::filesystem::path& file, std::error_code& ec);

}  // namespace crucible
