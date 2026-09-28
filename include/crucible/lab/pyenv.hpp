// SPDX-License-Identifier: MIT
//
// The Python environment the trainer runs in.
//
// Fine-tuning is the one thing Crucible cannot do in its own process. The
// adapter libraries -- peft, bitsandbytes, the torch build that matches the
// card -- are Python, they are large, and they are versioned against each
// other in ways that a C++ program has no business reimplementing. So the
// trainer is a script, and this is what puts a Python underneath it.
//
// Private, not shared. It lives in Crucible's data directory and is built
// from whatever Python the machine already has, which means installing it
// cannot break a system package, cannot be broken by one, and is removed by
// deleting a folder. It is not `pip install` into whatever interpreter was on
// PATH, which is how people end up with a torch that does not match their
// driver and no way to tell.
//
// Installed by the installer rather than on first use. The runtime modules
// next door are the other way round on purpose -- which GPU backend to build
// is a question only the machine can answer, and it is asked from the
// settings screen. This is not that question. There is one training
// environment, its shape follows from the hardware, and a fine-tune that
// stops to download four gigabytes is a fine-tune nobody starts. See
// runtime/builder.hpp for the other side of that argument.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "crucible/util/subprocess.hpp"

namespace crucible::lab::pyenv {

/// Which build of the training stack a machine wants.
///
/// The choice is made once, when the environment is created, and recorded:
/// the wheels differ by gigabytes and a CUDA torch on a machine with no
/// NVIDIA driver imports and then fails at the first tensor, which is a
/// confusing place to find out.
enum class Flavor {
    Cuda,  ///< NVIDIA, with bitsandbytes, so QLoRA works
    Cpu,   ///< no GPU, or one nothing here can train on. LoRA only, and slow
    Mlx,   ///< Apple Silicon, through mlx_lm rather than torch
};

std::string_view flavor_id(Flavor flavor);
Flavor           flavor_from_id(std::string_view id);

/// What this machine should get, from the hardware that is actually here.
Flavor flavor_here();

/// One line saying what a flavor can do, for the screen that offers it.
std::string_view flavor_note(Flavor flavor);

/// Roughly what installing `flavor` downloads, and what it leaves on disk.
///
/// Both are worth saying out loud before starting: the CUDA stack is a few
/// gigabytes down the wire and several more unpacked, and somebody on a
/// metered connection is entitled to know that before it begins rather than
/// after.
std::uint64_t download_bytes(Flavor flavor);
std::uint64_t installed_bytes(Flavor flavor);

/// One pip invocation: the packages, and the index to take them from.
///
/// Several rather than one, because torch comes from its own index and
/// everything else comes from PyPI. Pointing a single resolve at both leaves
/// pip free to take transformers from the torch mirror, where the version it
/// finds is whatever happened to be vendored.
struct Step {
    std::string              label;      ///< "PyTorch (CUDA)", for the progress line
    std::vector<std::string> packages;
    std::string              index_url;  ///< empty for PyPI
};

std::vector<Step> plan(Flavor flavor);

/// Every package name the environment should end up holding, for verifying.
std::vector<std::string> imports(Flavor flavor);

// --- where it all lives -----------------------------------------------------

std::filesystem::path root();           ///< data_dir()/trainer
std::filesystem::path venv_dir();       ///< root()/venv
std::filesystem::path python();         ///< the interpreter inside the venv
std::filesystem::path script();         ///< root()/finetune.py
std::filesystem::path manifest_file();  ///< root()/trainer.json
std::filesystem::path log_file();       ///< root()/install.log
std::filesystem::path runs_dir();       ///< root()/runs

/// A Python on this machine good enough to build the environment from, or an
/// empty path. `version` is filled in either way, so a refusal can say what
/// was found rather than only that it was not good enough.
///
/// 3.10 is the floor: it is what current torch, transformers and peft all
/// still publish wheels for, and below it pip resolves to versions from
/// before any of this worked.
std::filesystem::path host_python(std::string& version);

/// The advice for a machine with no usable Python, phrased for its package
/// manager. Empty when one was found.
std::string python_hint();

// --- what is installed ------------------------------------------------------

/// What is actually on disk, measured rather than remembered.
struct Status {
    bool present = false;  ///< the folder and its interpreter are there
    bool ready   = false;  ///< ...and everything the trainer imports, imports

    Flavor      flavor = Flavor::Cpu;
    std::string python_version;
    std::string torch_version;   ///< or the mlx version, on a Mac
    std::string installed_at;

    /// Why `ready` is false. Empty when it is true.
    std::string note;

    /// Cards this torch was built for, and cards it was not.
    ///
    /// The second list is the one worth having. A torch wheel carries
    /// kernels for a fixed set of architectures and a newer card is not
    /// slow, it is unusable -- but the import succeeds and
    /// `torch.cuda.is_available()` still says true, so nothing short of
    /// asking finds out. On a machine with three cards the run simply uses
    /// the two that work and nobody notices the third is idle.
    std::vector<std::string> usable_gpus;
    std::vector<std::string> unusable_gpus;

    std::uint64_t bytes = 0;
};

Status status();

/// Is there an environment at all?
///
/// A look at the filesystem and nothing more. `status()` starts a Python and
/// imports torch, which is a second or two -- fine for a settings page that
/// was opened to look at it, far too slow for a tab that only wants to warn
/// somebody before they press Train.
bool looks_installed();

/// Delete the environment, its script and its runs.
bool remove(std::string& error);

// --- installing it ----------------------------------------------------------

/// Where an install has got to. Copied under a lock, a frame at a time.
struct Progress {
    enum class Phase {
        Idle,
        FindingPython,  ///< looking for an interpreter to build from
        CreatingVenv,   ///< python -m venv
        Installing,     ///< the long one: pip, several gigabytes
        Verifying,      ///< importing everything the trainer will import
        Done,
        Failed,
        Canceled,
    };

    Phase       phase   = Phase::Idle;
    Flavor      flavor  = Flavor::Cpu;
    float       percent = 0.0F;  ///< 0..1 across the pip steps
    std::string step;            ///< the package being fetched
    std::string error;

    std::vector<std::string> log_tail;
    std::filesystem::path    log_file;

    bool finished() const {
        return phase == Phase::Done || phase == Phase::Failed || phase == Phase::Canceled;
    }
    bool running() const { return phase != Phase::Idle && !finished(); }

    /// "installing PyTorch (CUDA) 40%" -- one line for the settings screen.
    std::string label() const;
};

/// Builds the environment on a thread of its own.
///
/// The same shape as RuntimeBuilder next door, for the same reason: this
/// takes minutes, the window must keep drawing, and the user must be able to
/// give up. Consistency is worth more here than the few lines sharing a base
/// class would save.
class Installer {
public:
    Installer() = default;
    ~Installer();
    Installer(const Installer&)            = delete;
    Installer& operator=(const Installer&) = delete;

    /// Start installing `flavor`. `on_change` is called from the worker
    /// whenever progress moved and must be safe off the UI thread. Returns
    /// false when one is already running.
    bool start(Flavor flavor, std::function<void()> on_change);

    /// Ask the install to stop. pip is signaled, so it takes effect in about
    /// a second rather than at the end of a 2 GB download.
    void cancel();

    /// Cancel and wait for the worker to be gone. Teardown blocks here: the
    /// worker calls back into the application.
    void stop();

    /// Forget a finished install, returning to Idle.
    void dismiss();

    Progress progress() const;

private:
    void run(Flavor flavor);

    void set_phase(Progress::Phase phase, std::string step = {});
    void fail(std::string error);

    /// Run one child to completion, logging every line. `weight` is how much
    /// of the whole install this command accounts for, for the bar.
    bool run_command(const std::vector<std::string>& argv, float from, float to);

    mutable std::mutex       mutex_;
    Progress                 progress_;
    std::vector<std::string> log_;
    std::thread              worker_;
    std::atomic<bool>        cancel_{false};
    std::function<void()>    on_change_;

    /// The running child, so cancel() can reach it while the worker is
    /// blocked reading from it. Its own lock: the worker holds it briefly and
    /// the UI thread must not wait behind a log write to get at it.
    mutable std::mutex                    child_mutex_;
    std::unique_ptr<util::Subprocess>     child_;
};

/// Write the trainer script out beside the environment.
///
/// Compiled into the binary rather than installed as a data file: an AppImage,
/// a .app bundle and a Windows install put their data in three different
/// places, and a script that is always exactly the one this build expects is
/// worth more than a file somebody could edit. Rewritten on every install, so
/// upgrading Crucible upgrades the trainer.
bool write_script(std::string& error);

}  // namespace crucible::lab::pyenv
