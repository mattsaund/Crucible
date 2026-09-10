// SPDX-License-Identifier: MIT
//
// Compiling a ggml backend on demand. See builder.hpp for the shape of it.
#include "crucible/runtime/builder.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string_view>

#include <nlohmann/json.hpp>

#include "crucible/config/paths.hpp"
#include "crucible/runtime/registry.hpp"
#include "crucible/util/format.hpp"

namespace crucible {
namespace {

using json = nlohmann::json;

/// The llama.cpp tag this binary was compiled against. A backend built from a
/// different tag would load and then crash on the first tensor: ggml's
/// internal structures are not stable across releases. CMake passes the tag
/// in so there is exactly one place to change it.
#ifndef CRUCIBLE_LLAMA_TAG
#define CRUCIBLE_LLAMA_TAG "unknown"
#endif

/// Keep the log bounded. A CUDA build emits tens of thousands of lines, and
/// the only ones anybody reads are the last few before it stopped.
constexpr std::size_t kMaxLogLines = 4000;
constexpr std::size_t kTailLines   = 25;

std::string iso_date_now() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
    ::gmtime_r(&now, &parts);
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M UTC", &parts);
    return buffer;
}

/// Pull the percentage out of a cmake/make progress line like
/// "[ 42%] Building CXX object ...". Returns -1 when the line is not one.
int parse_build_percent(const std::string& line, std::string& step) {
    if (line.size() < 4 || line.front() != '[') {
        return -1;
    }
    const std::size_t close = line.find(']');
    if (close == std::string::npos) {
        return -1;
    }
    const std::size_t percent_sign = line.rfind('%', close);
    if (percent_sign == std::string::npos || percent_sign > close) {
        return -1;
    }

    std::string digits = line.substr(1, percent_sign - 1);
    digits.erase(std::remove_if(digits.begin(), digits.end(),
                                [](unsigned char c) { return std::isspace(c) != 0; }),
                 digits.end());
    if (digits.empty() ||
        !std::all_of(digits.begin(), digits.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return -1;
    }

    step = line.size() > close + 2 ? line.substr(close + 2) : std::string();
    // The path is usually far wider than the settings pane; the file name
    // alone is what tells the user the build is moving.
    if (const std::size_t slash = step.rfind('/'); slash != std::string::npos) {
        step = step.substr(slash + 1);
    }
    return std::stoi(digits);
}

/// How many compile jobs to run. Builds are the background task here, not the
/// foreground one, so leave the machine usable: the user is still typing at a
/// TUI while this runs.
std::string job_count() {
    const unsigned int cores = std::max(1U, std::thread::hardware_concurrency());
    return std::to_string(std::max(1U, cores > 2 ? cores - 1 : cores));
}

/// Is the CPU runtime already installed? Read from the directory rather than
/// from ggml's registry, because a build started before any runtime was
/// loaded still has to see one that was installed a minute ago.
bool cpu_installed() {
    for (const RuntimeStatus& runtime : RuntimeRegistry::scan()) {
        if (runtime.kind == BackendKind::Cpu) {
            return runtime.installed;
        }
    }
    return false;
}

/// Does `filename` belong to one of the backends this build set out to make?
/// Matched on the module prefix, since the CPU backend arrives as a dozen
/// files (libggml-cpu-haswell.so and friends) rather than one.
bool produces_module(const std::vector<BackendKind>& produces, const std::string& filename) {
    return std::any_of(produces.begin(), produces.end(), [&filename](BackendKind kind) {
        return filename.rfind(std::string(module_prefix()) + std::string(backend_info(kind).id),
                              0) == 0;
    });
}

/// Every line a program writes, or nothing if it could not be run.
///
/// For asking short questions of the toolchain -- what does this driver see,
/// what can this compiler target -- rather than for driving a build, which is
/// what run_command is for.
std::vector<std::string> ask(const std::vector<std::string>& argv) {
    if (!util::on_path(argv.front())) {
        return {};
    }
    util::Subprocess child;
    std::string      error;
    if (!child.start(argv, {}, /*extra_env=*/{}, error)) {
        return {};
    }
    std::vector<std::string> lines;
    std::string              line;
    while (child.read_line(line)) {
        lines.push_back(line);
    }
    return child.wait() == 0 ? lines : std::vector<std::string>{};
}

/// A compute capability as CMake writes it: "8.9" becomes 89.
int capability_from(std::string_view text) {
    int major = 0;
    int minor = 0;
    if (std::sscanf(std::string(text).c_str(), "%d.%d", &major, &minor) != 2) {
        return 0;
    }
    return major * 10 + minor;
}

/// What this machine's cards and compiler can do, for cuda_architectures.
///
/// Two questions of the toolchain, both cheap, both asked at configure time so
/// the answer is about the machine the build is happening on. Either one coming
/// back empty means the arithmetic cannot be trusted, and the caller falls back
/// to llama.cpp's defaults -- a slow build is a far better failure than a
/// module this machine cannot run.
std::string detected_cuda_architectures() {
    std::vector<int> present;
    for (const std::string& line :
         ask({"nvidia-smi", "--query-gpu=compute_cap", "--format=csv,noheader"})) {
        if (const int capability = capability_from(line); capability > 0) {
            present.push_back(capability);
        }
    }

    // "compute_50", "compute_52", ... one per line.
    std::vector<int> targetable;
    for (const std::string& line : ask({"nvcc", "--list-gpu-arch"})) {
        constexpr std::string_view kPrefix = "compute_";
        if (line.rfind(kPrefix, 0) == 0) {
            targetable.push_back(std::atoi(line.c_str() + kPrefix.size()));
        }
    }
    return cuda_architectures(present, targetable);
}

/// Does `filename` end in this platform's loadable-module extension?
bool is_module_file(const std::string& filename) {
    const std::string_view suffix = module_suffix();
    return filename.size() > suffix.size() &&
           filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// Write the CMake fragment that stops llama.cpp writing versioned shared
/// libraries, and return its path.
///
/// Crucible's own build does this by calling crucible_unversion_directory() after
/// FetchContent has added llama.cpp. This build cannot: it runs cmake on
/// llama.cpp directly, with no Crucible CMakeLists in the picture. So the
/// fragment goes in through CMAKE_PROJECT_INCLUDE, which cmake reads at the
/// end of each project() call -- before any target exists, hence the deferred
/// call, which runs once the whole tree has been processed.
///
/// It is written into the build directory rather than installed beside the
/// binary: it is fifteen lines, it belongs to exactly one build, and a file
/// the install could be missing is a failure mode this does not need.
///
/// Why at all: llama.cpp writes libggml-cuda.so.0.9.4 with libggml-cuda.so as
/// a symlink beside it, and neither exFAT nor NTFS can hold a symlink. That
/// makes the build fail outright on a machine whose home directory is on one.
/// See cmake/CrucibleUnversion.cmake.
std::filesystem::path write_unversion_hook(const std::filesystem::path& build) {
    const std::filesystem::path hook = build / "crucible-unversion.cmake";
    std::ofstream out(hook);
    if (!out) {
        return {};
    }
    out << R"cmake(# Written by Crucible's runtime builder. See src/runtime/builder.cpp.
function(crucible_unversion_directory dir)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
        get_target_property(_type ${_target} TYPE)
        if(_type STREQUAL "SHARED_LIBRARY" OR _type STREQUAL "MODULE_LIBRARY")
            set_property(TARGET ${_target} PROPERTY VERSION)
            set_property(TARGET ${_target} PROPERTY SOVERSION)
            set_property(TARGET ${_target} PROPERTY MACHO_CURRENT_VERSION)
            set_property(TARGET ${_target} PROPERTY MACHO_COMPATIBILITY_VERSION)
        endif()
    endforeach()
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_subdir IN LISTS _subdirs)
        crucible_unversion_directory("${_subdir}")
    endforeach()
endfunction()

# This file is included once per project() call; only the first one arranges
# the sweep, and it is deferred so that every target exists by the time it runs.
if(NOT DEFINED CRUCIBLE_UNVERSION_DEFERRED)
    set(CRUCIBLE_UNVERSION_DEFERRED ON)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
                   CALL crucible_unversion_directory "${CMAKE_SOURCE_DIR}")
endif()
)cmake";
    return out ? hook : std::filesystem::path{};
}

}  // namespace

std::string cuda_architectures(const std::vector<int>& present,
                               const std::vector<int>& targetable) {
    if (present.empty() || targetable.empty()) {
        return {};
    }

    std::vector<int> cards = present;
    std::sort(cards.begin(), cards.end());
    cards.erase(std::unique(cards.begin(), cards.end()), cards.end());

    const int highest_targetable = *std::max_element(targetable.begin(), targetable.end());

    std::string architectures;
    int         newest = 0;
    for (const int capability : cards) {
        if (std::find(targetable.begin(), targetable.end(), capability) != targetable.end()) {
            if (!architectures.empty()) {
                architectures += ";";
            }
            architectures += std::to_string(capability) + "-real";
            newest = std::max(newest, capability);
        } else if (capability > highest_targetable) {
            // Newer than this toolkit knows about, so there is no real code to
            // emit. PTX for the newest thing it does know is what the driver
            // will compile from.
            newest = std::max(newest, highest_targetable);
        }
        // A card older than the toolkit supports is left out: there is nothing
        // to emit for it, and llama.cpp's defaults would not have helped either.
    }
    if (newest == 0) {
        return {};
    }
    if (!architectures.empty()) {
        architectures += ";";
    }
    return architectures + std::to_string(newest) + "-virtual";
}

std::string BuildProgress::label() const {
    switch (phase) {
        case Phase::Idle:           return "idle";
        case Phase::FetchingSource: return "fetching llama.cpp source";
        case Phase::Configuring:    return "configuring";
        case Phase::Compiling:
            return "compiling " + format::number(static_cast<double>(percent) * 100.0, 0) + "%";
        case Phase::Installing:     return "installing";
        case Phase::Done:           return "installed";
        case Phase::Failed:         return "failed";
        case Phase::Canceled:      return "canceled";
    }
    return {};
}

RuntimeBuilder::~RuntimeBuilder() { stop(); }

void RuntimeBuilder::stop() {
    cancel();
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false);
}

BuildProgress RuntimeBuilder::progress() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return progress_;
}

bool RuntimeBuilder::start(BackendKind kind, std::function<void()> on_change) {
    if (running_.exchange(true)) {
        return false;
    }
    if (worker_.joinable()) {
        worker_.join();  // reap the previous, finished, build
    }

    cancel_.store(false);
    on_change_ = std::move(on_change);
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        progress_          = BuildProgress{};
        progress_.kind     = kind;
        progress_.phase    = BuildProgress::Phase::Configuring;
        progress_.started  = std::chrono::steady_clock::now();
        progress_.log_file = paths::data_dir() / ("runtime-build-" +
                                                  std::string(backend_info(kind).id) + ".log");
        log_.clear();
    }

    worker_ = std::thread([this, kind] { run(kind); });
    return true;
}

void RuntimeBuilder::cancel() {
    cancel_.store(true);
    const std::lock_guard<std::mutex> lock(child_mutex_);
    if (child_) {
        child_->terminate();
    }
}

void RuntimeBuilder::dismiss() {
    if (running_.load()) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    progress_ = BuildProgress{};
}

void RuntimeBuilder::set_phase(BuildProgress::Phase phase, std::string step) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        progress_.phase = phase;
        progress_.step  = std::move(step);
    }
    if (on_change_) {
        on_change_();
    }
}

void RuntimeBuilder::fail(std::string error) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        progress_.phase = BuildProgress::Phase::Failed;
        progress_.error = std::move(error);
        progress_.log_tail.assign(
            log_.size() > kTailLines ? log_.end() - static_cast<long>(kTailLines) : log_.begin(),
            log_.end());
    }
    if (on_change_) {
        on_change_();
    }
}

bool RuntimeBuilder::run_command(const std::vector<std::string>& argv,
                                 const std::filesystem::path& cwd,
                                 BuildProgress::Phase phase,
                                 bool parse_percent) {
    set_phase(phase);

    auto child = std::make_unique<util::Subprocess>();
    std::string error;
    if (!child->start(argv, cwd, /*extra_env=*/{}, error)) {
        fail(argv[0] + ": " + error);
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(child_mutex_);
        child_ = std::move(child);
    }

    std::string line;
    int last_percent = -1;
    for (;;) {
        util::Subprocess* running = nullptr;
        {
            const std::lock_guard<std::mutex> lock(child_mutex_);
            running = child_.get();
        }
        if (running == nullptr || !running->read_line(line)) {
            break;
        }

        {
            const std::lock_guard<std::mutex> lock(mutex_);
            log_.push_back(line);
            if (log_.size() > kMaxLogLines) {
                log_.erase(log_.begin(), log_.begin() + static_cast<long>(kMaxLogLines / 4));
            }
        }

        if (parse_percent) {
            std::string step;
            const int percent = parse_build_percent(line, step);
            // Redraw on each new percent rather than each line: a build emits
            // thousands of lines and the screen only has a hundred pixels of
            // progress bar to show for them.
            if (percent >= 0 && percent != last_percent) {
                last_percent = percent;
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    progress_.percent = static_cast<float>(percent) / 100.0F;
                    progress_.step    = step;
                }
                if (on_change_) {
                    on_change_();
                }
            }
        }
    }

    int status = -1;
    {
        const std::lock_guard<std::mutex> lock(child_mutex_);
        if (child_) {
            status = child_->wait();
            child_.reset();
        }
    }

    // Write the log out whatever happened: a successful build that produced
    // warnings is still worth being able to read afterwards.
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::ofstream out(progress_.log_file, std::ios::app);
        for (const std::string& entry : log_) {
            out << entry << '\n';
        }
    }

    if (cancel_.load()) {
        return false;
    }
    if (status != 0) {
        fail(argv[0] + " exited with status " + std::to_string(status));
        return false;
    }
    return true;
}

bool RuntimeBuilder::ensure_source(std::string& error) {
    const std::filesystem::path src = paths::runtime_src_dir();
    std::error_code ec;

    if (std::filesystem::exists(src / "CMakeLists.txt", ec)) {
        return true;
    }

    set_phase(BuildProgress::Phase::FetchingSource, "cloning llama.cpp " CRUCIBLE_LLAMA_TAG);
    std::filesystem::create_directories(src.parent_path(), ec);
    std::filesystem::remove_all(src, ec);

    if (!util::on_path("git")) {
        error = "git is needed to fetch the llama.cpp source and is not installed";
        return false;
    }

    const bool ok = run_command({"git", "clone", "--depth", "1", "--branch", CRUCIBLE_LLAMA_TAG,
                                 "https://github.com/ggml-org/llama.cpp.git", src.string()},
                                {}, BuildProgress::Phase::FetchingSource, false);
    if (!ok) {
        error = "could not clone llama.cpp " CRUCIBLE_LLAMA_TAG;
        return false;
    }
    return true;
}

void RuntimeBuilder::run(BackendKind kind) {
    const BackendInfo& info = backend_info(kind);

    // Check the SDK first. A CUDA build that fails on a missing nvcc after
    // four minutes of configuring teaches the user nothing they could not have
    // been told immediately.
    if (!info.required_tool.empty() && !util::on_path(std::string(info.required_tool))) {
        const std::string hint = install_hint(info);
        fail(std::string(info.required_tool) + " is not on PATH." +
             (hint.empty() ? "" : "  Install it with:  " + hint));
        running_.store(false);
        return;
    }

    std::string error;
    if (!ensure_source(error)) {
        set_phase(cancel_.load() ? BuildProgress::Phase::Canceled : BuildProgress::Phase::Failed);
        if (!cancel_.load()) {
            fail(error);
        }
        running_.store(false);
        return;
    }

    const std::filesystem::path src   = paths::runtime_src_dir();
    const std::filesystem::path build = paths::runtime_build_dir() / std::string(info.id);

    std::error_code ec;
    std::filesystem::create_directories(build, ec);

    const std::filesystem::path unversion = write_unversion_hook(build);

    // What this build will produce. Usually just the backend that was asked
    // for -- but llama.cpp cannot load a model without the CPU backend even
    // when every layer is going to a GPU, so a first CUDA install on a machine
    // with an empty runtimes directory has to bring it along or it would
    // install a runtime that still cannot answer anything. See
    // BackendInfo::required.
    std::vector<BackendKind> produces{kind};
    const bool with_cpu = kind != BackendKind::Cpu && !cpu_installed();
    if (with_cpu) {
        produces.push_back(BackendKind::Cpu);
    }

    if (!cancel_.load()) {
        // Only the backend modules are wanted, so every other part of
        // llama.cpp is switched off.
        std::vector<std::string> configure = {
            "cmake", "-S", src.string(), "-B", build.string(),
            "-DCMAKE_BUILD_TYPE=Release",
            "-DBUILD_SHARED_LIBS=ON",
            "-DGGML_BACKEND_DL=ON",
            "-DGGML_NATIVE=OFF",
            "-D" + std::string(info.cmake_option) + "=ON",
            "-DLLAMA_BUILD_TESTS=OFF",
            "-DLLAMA_BUILD_EXAMPLES=OFF",
            "-DLLAMA_BUILD_TOOLS=OFF",
            "-DLLAMA_BUILD_SERVER=OFF",
            "-DLLAMA_BUILD_COMMON=OFF",
            "-DLLAMA_CURL=OFF",
        };
        if (!unversion.empty()) {
            configure.emplace_back("-DCMAKE_PROJECT_INCLUDE=" + unversion.string());
        }
        // A runtime build produces exactly the backend that was asked for. On a
        // Mac that has to be said out loud: ggml turns Metal and BLAS on by
        // itself under APPLE, so asking for the CPU runtime would quietly build
        // three modules and install all of them -- and `produces` below, which
        // decides what the install verifies, would not know about two of them.
        if (kind != BackendKind::Metal) {
            configure.emplace_back("-DGGML_METAL=OFF");
        }
        configure.emplace_back("-DGGML_BLAS=OFF");
        configure.emplace_back("-DGGML_ACCELERATE=OFF");
        if (kind == BackendKind::Metal) {
            // Not a default worth trusting.
            //
            // ggml can put the Metal shaders in a `default.metallib` beside the
            // module instead of inside it, and the install here copies the
            // module and nothing else -- deliberately, because everything else
            // a build leaves in bin/ is a duplicate of it. A Metal backend that
            // left its shaders behind would install cleanly and then fail on
            // the first tensor, which is the worst shape a failure can take.
            configure.emplace_back("-DGGML_METAL_EMBED_LIBRARY=ON");
        }
        if (kind == BackendKind::Cuda) {
            // The single biggest thing this build spends time on. See
            // cuda_architectures.
            if (const std::string architectures = detected_cuda_architectures();
                !architectures.empty()) {
                configure.emplace_back("-DCMAKE_CUDA_ARCHITECTURES=" + architectures);
            }
        }
        if (kind == BackendKind::Cpu || with_cpu) {
            // One module per x86-64 feature level, scored at load time. This
            // is the only way to build the CPU backend in DL mode: GGML_NATIVE
            // is rejected here, so without it every module would be compiled
            // for the lowest common denominator.
            configure.emplace_back("-DGGML_CPU=ON");
            configure.emplace_back("-DGGML_CPU_ALL_VARIANTS=ON");
        } else {
            // The CPU runtime is already installed, and compiling it again
            // would add minutes for a module that would just overwrite itself.
            configure.emplace_back("-DGGML_CPU=OFF");
        }
        if (!run_command(configure, {}, BuildProgress::Phase::Configuring, false)) {
            if (cancel_.load()) {
                set_phase(BuildProgress::Phase::Canceled);
                running_.store(false);
                return;
            }

            // Configure failed with a build directory already there. By far
            // the most likely reason is a CMakeCache.txt that remembers a path
            // this tree no longer lives at -- a home directory that moved, a
            // restored backup, a prefix that changed -- and cmake refuses to
            // reuse it. The cache is pure derived data, so throwing it away
            // and trying once more is both safe and the fix the user would
            // otherwise have to find out about from a log.
            std::error_code retry_ec;
            if (std::filesystem::exists(build / "CMakeCache.txt", retry_ec)) {
                set_phase(BuildProgress::Phase::Configuring, "clearing a stale build directory");
                std::filesystem::remove_all(build, retry_ec);
                std::filesystem::create_directories(build, retry_ec);
                // The hook went with the directory.
                write_unversion_hook(build);
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    log_.clear();
                }
                if (!run_command(configure, {}, BuildProgress::Phase::Configuring, false)) {
                    if (cancel_.load()) {
                        set_phase(BuildProgress::Phase::Canceled);
                    }
                    running_.store(false);
                    return;
                }
            } else {
                running_.store(false);
                return;
            }
        }
    }

    if (!cancel_.load()) {
        // The target is `ggml`, not the backend itself. ggml makes every
        // enabled backend a dependency of that target, so this builds exactly
        // the modules this configuration turned on -- and it is the only name
        // that works for the CPU backend, which under GGML_CPU_ALL_VARIANTS is
        // a dozen targets (ggml-cpu-haswell, ggml-cpu-zen4, ...) and no single
        // `ggml-cpu` at all.
        const std::vector<std::string> compile = {
            "cmake", "--build", build.string(),
            "--target", "ggml",
            "-j", job_count(),
        };
        if (!run_command(compile, {}, BuildProgress::Phase::Compiling, true)) {
            if (cancel_.load()) {
                set_phase(BuildProgress::Phase::Canceled);
            }
            running_.store(false);
            return;
        }
    }

    if (cancel_.load()) {
        set_phase(BuildProgress::Phase::Canceled);
        running_.store(false);
        return;
    }

    // --- install -----------------------------------------------------------
    set_phase(BuildProgress::Phase::Installing);
    const std::filesystem::path target = paths::runtimes_dir();
    std::filesystem::create_directories(target, ec);

    int copied = 0;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(build / "bin", ec)) {
        const std::string name = entry.path().filename().string();
        if (!produces_module(produces, name)) {
            continue;
        }
        // ggml opens a module by an exact file name, so only the module
        // itself is wanted. Anything else the build left in bin/ -- an import
        // library, a debug file, an alias from an older llama.cpp that still
        // versioned its libraries -- would just be a copy of it under another
        // name, which for a 400 MB CUDA module is worth not making.
        if (!is_module_file(name)) {
            continue;
        }

        // Copy to a temporary name and rename over the target, rather than
        // writing the destination in place.
        //
        // Reinstalling a runtime that is already loaded is an ordinary thing
        // to do -- it is how you rebuild one after a driver update -- and by
        // then the module is dlopen'd and mmap'd into this process.
        // Overwriting those bytes underneath the mapping crashes Crucible on the
        // next call into the backend. rename() swaps the directory entry
        // instead: the running process keeps the old inode, and the next start
        // picks up the new one.
        const std::filesystem::path final_path = target / name;
        const std::filesystem::path staged     = target / (name + ".new");
        std::filesystem::remove(staged, ec);
        ec.clear();
        std::filesystem::copy_file(entry.path(), staged,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            std::filesystem::rename(staged, final_path, ec);
        }
        if (ec) {
            std::error_code cleanup_ec;
            std::filesystem::remove(staged, cleanup_ec);
            fail("could not install " + name + ": " + ec.message());
            running_.store(false);
            return;
        }
        ++copied;
    }

    if (copied == 0) {
        fail("the build produced no " + std::string(info.build_target) + " module");
        running_.store(false);
        return;
    }

    // --- record what was built ---------------------------------------------
    const std::filesystem::path manifest_path = target / "manifest.json";
    json manifest = json::object();
    if (std::ifstream in(manifest_path); in) {
        json parsed = json::parse(in, nullptr, false);
        if (parsed.is_object()) {
            manifest = std::move(parsed);
        }
    }
    const std::string built_at = iso_date_now();
    for (const BackendKind produced : produces) {
        manifest[std::string(backend_info(produced).id)] = {
            {"llama_tag", CRUCIBLE_LLAMA_TAG},
            {"built_at", built_at},
        };
    }
    if (std::ofstream out(manifest_path); out) {
        out << manifest.dump(2) << '\n';
    }

    // Register the new modules with ggml straight away.
    //
    // Without this the build finishes and nothing changes until Crucible is
    // restarted, which is confusing enough that it reads as the install having
    // silently failed. The CPU backend goes first: llama.cpp looks for it by
    // type and a GPU registered ahead of it does not stand in.
    std::sort(produces.begin(), produces.end(), [](BackendKind a, BackendKind b) {
        return backend_info(a).required && !backend_info(b).required;
    });
    std::string activate_error;
    for (const BackendKind produced : produces) {
        if (!RuntimeRegistry::activate(produced, activate_error)) {
            const std::lock_guard<std::mutex> lock(mutex_);
            progress_.error = activate_error;
            break;
        }
    }

    set_phase(BuildProgress::Phase::Done);
    running_.store(false);
}

}  // namespace crucible
