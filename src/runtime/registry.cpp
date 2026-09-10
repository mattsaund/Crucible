// SPDX-License-Identifier: MIT
//
// Scanning, loading and removing the runtimes directory.
#include "crucible/runtime/registry.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <optional>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

#include <ggml-backend.h>

#include "crucible/config/paths.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/subprocess.hpp"

namespace crucible {
namespace {

using json = nlohmann::json;

/// ggml names its modules libggml-<backend>[-<variant>].so, so the backend a
/// file belongs to is the first component after the prefix. The CPU backend
/// ships as a dozen variants (haswell, icelake, ...) that all belong to the
/// same runtime, which is why this maps a file to a *backend* rather than
/// expecting one file per backend.
///
/// The prefix and extension are ggml's, not ours -- see module_prefix().
std::optional<BackendKind> kind_of_module(const std::filesystem::path& file) {
    const std::string name = file.filename().string();

    const std::string_view prefix = module_prefix();
    const std::string_view suffix = module_suffix();
    if (name.rfind(prefix, 0) != 0) {
        return std::nullopt;
    }
    // Only a file ending in exactly the module extension counts. Anything
    // else in the directory -- a half-copied ".new", a leftover versioned
    // alias from an older Crucible -- is the same module under another name,
    // and counting it would double every size shown in settings.
    if (name.size() <= prefix.size() + suffix.size() ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return std::nullopt;
    }

    std::string rest = name.substr(prefix.size());
    rest.erase(rest.size() - suffix.size());

    const std::size_t dash = rest.find('-');
    if (dash != std::string::npos) {
        rest.erase(dash);
    }
    return backend_from_id(rest);
}

/// Every module file in `dir`, grouped by backend.
std::map<BackendKind, std::vector<std::filesystem::path>> modules_in(
    const std::filesystem::path& dir) {
    std::map<BackendKind, std::vector<std::filesystem::path>> found;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return found;
    }
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_directory()) {
            continue;
        }
        if (const std::optional<BackendKind> kind = kind_of_module(entry.path())) {
            found[*kind].push_back(entry.path());
        }
    }
    for (auto& [kind, files] : found) {
        std::sort(files.begin(), files.end());
    }
    return found;
}

/// How many devices ggml registered for each backend it managed to load.
std::map<BackendKind, int> registered_devices() {
    std::map<BackendKind, int> counts;
    const std::size_t total = ggml_backend_dev_count();
    for (std::size_t i = 0; i < total; ++i) {
        ggml_backend_dev_t device = ggml_backend_dev_get(i);
        if (device == nullptr) {
            continue;
        }
        ggml_backend_reg_t reg = ggml_backend_dev_backend_reg(device);
        if (reg == nullptr) {
            continue;
        }
        if (const std::optional<BackendKind> kind =
                backend_from_reg_name(ggml_backend_reg_name(reg))) {
            ++counts[*kind];
        }
    }
    return counts;
}

/// Which of a backend's modules this machine should actually run.
///
/// The CPU backend ships as one module per x86-64 feature level and only one
/// of them may be loaded, so the choice is ggml's own: each module exports
/// `ggml_backend_score`, which returns 0 when the CPU cannot run it and a
/// higher number the more of the machine it uses. ggml does this at startup
/// inside ggml_backend_load_all_from_path; a runtime installed later has to
/// have it done for it.
///
/// dlopen here is a probe, not a load: the winner is closed again and handed
/// to ggml_backend_load, which is the call that registers it.
std::filesystem::path best_module(const std::vector<std::filesystem::path>& files) {
    if (files.size() == 1) {
        return files.front();
    }

    std::filesystem::path best;
    int best_score = 0;
    for (const std::filesystem::path& file : files) {
#if defined(_WIN32)
        HMODULE handle = ::LoadLibraryW(file.wstring().c_str());
        if (handle == nullptr) {
            continue;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        auto score_fn =
            reinterpret_cast<int (*)()>(::GetProcAddress(handle, "ggml_backend_score"));
        const int score = score_fn != nullptr ? score_fn() : 1;
        ::FreeLibrary(handle);
#else
        void* handle = ::dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            continue;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        auto score_fn = reinterpret_cast<int (*)()>(::dlsym(handle, "ggml_backend_score"));
        const int score = score_fn != nullptr ? score_fn() : 1;
        ::dlclose(handle);
#endif
        if (score > best_score) {
            best_score = score;
            best        = file;
        }
    }
    return best;
}

json read_manifest(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) {
        return json::object();
    }
    json parsed = json::parse(in, nullptr, /*allow_exceptions=*/false);
    return parsed.is_object() ? parsed : json::object();
}

}  // namespace

/// The llama.cpp tag this binary was compiled against. CMake passes it in; the
/// fallback keeps a hand-rolled build compiling rather than failing here.
#ifndef CRUCIBLE_LLAMA_TAG
#define CRUCIBLE_LLAMA_TAG "unknown"
#endif

std::string_view RuntimeStatus::required_llama_tag() {
    return CRUCIBLE_LLAMA_TAG;
}

std::string RuntimeStatus::size_label() const {
    return format::bytes(bytes);
}

std::filesystem::path RuntimeRegistry::manifest_file() {
    return paths::runtimes_dir() / "manifest.json";
}

bool RuntimeRegistry::loadable_backends_supported() {
#ifdef GGML_BACKEND_DL
    return true;
#else
    return false;
#endif
}

bool RuntimeRegistry::any_installed() {
    return !modules_in(paths::runtimes_dir()).empty();
}

bool RuntimeRegistry::activate(BackendKind kind, std::string& error) {
    if (!loadable_backends_supported()) {
        error = "this build has its backend compiled in; runtimes cannot be loaded";
        return false;
    }

    // ggml's registry is the source of truth for "already loaded". Asking it
    // rather than remembering ourselves is what makes this safe to call after
    // every build: load_backend() has no duplicate check of its own, and a
    // second registration would give every device an identical twin.
    if (registered_devices().count(kind) != 0) {
        return true;
    }

    const auto installed = modules_in(paths::runtimes_dir());
    const auto found = installed.find(kind);
    if (found == installed.end() || found->second.empty()) {
        error = std::string(backend_info(kind).name) + " is not installed";
        return false;
    }

    const std::filesystem::path best = best_module(found->second);
    if (best.empty()) {
        error = std::string(backend_info(kind).name) +
                " will not run on this machine (no module scored above zero)";
        return false;
    }

    if (ggml_backend_load(best.string().c_str()) == nullptr) {
        error = "could not load " + best.filename().string() +
                " -- see the Crucible log";
        return false;
    }
    return true;
}

void RuntimeRegistry::load_all() {
    if (!loadable_backends_supported()) {
        return;  // the backend is compiled in; there is nothing to load
    }
    const std::string dir = paths::runtimes_dir().string();
    ggml_backend_load_all_from_path(dir.c_str());
}

std::vector<RuntimeStatus> RuntimeRegistry::scan() {
    const auto installed = modules_in(paths::runtimes_dir());
    const auto devices   = registered_devices();
    const json manifest  = read_manifest(manifest_file());

    std::vector<RuntimeStatus> result;
    result.reserve(kBackendCount);

    for (const BackendInfo& info : all_backends()) {
        // A backend this machine cannot have is not listed at all. Showing
        // CUDA on a Mac, grayed out with a reason, is a row that can never
        // become anything -- and the list is short enough that every row in it
        // should be one you could act on.
        if (!backend_available_here(info.kind)) {
            continue;
        }
        RuntimeStatus status;
        status.kind = info.kind;

        if (const auto found = installed.find(info.kind); found != installed.end()) {
            status.installed = true;
            status.files     = found->second;
            std::error_code ec;
            for (const std::filesystem::path& file : status.files) {
                status.bytes += std::filesystem::file_size(file, ec);
            }
        }

        if (const auto counted = devices.find(info.kind); counted != devices.end()) {
            status.active       = counted->second > 0;
            status.device_count = counted->second;
        }

        if (const auto entry = manifest.find(std::string(info.id)); entry != manifest.end()) {
            status.llama_tag = entry->value("llama_tag", "");
            status.built_at  = entry->value("built_at", "");
        }
        // An unrecorded tag is "cannot tell", not "wrong": a manifest can be
        // lost without the modules being any less valid.
        status.stale = status.installed && !status.llama_tag.empty() &&
                       status.llama_tag != RuntimeStatus::required_llama_tag();

        if (!info.required_tool.empty() && !util::on_path(std::string(info.required_tool))) {
            status.buildable = false;
            status.blocker   = std::string(info.required_tool) + " is not installed";
        }

        result.push_back(std::move(status));
    }
    return result;
}

bool RuntimeRegistry::remove(BackendKind kind, std::string& error) {
    const BackendInfo& info = backend_info(kind);

    const auto installed = modules_in(paths::runtimes_dir());
    const auto found = installed.find(kind);
    if (found == installed.end()) {
        error = std::string(info.name) + " is not installed";
        return false;
    }

    std::error_code ec;
    for (const std::filesystem::path& file : found->second) {
        // Take any versioned aliases with it. Crucible no longer produces them
        // (see cmake/CrucibleUnversion.cmake), but a runtime built by an older
        // one has libggml-cuda.so.0 and libggml-cuda.so.0.9.4 sitting beside
        // the module, and those would be left behind as several hundred
        // megabytes of orphan.
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(file.parent_path(), ec)) {
            const std::string name = entry.path().filename().string();
            if (name.rfind(file.filename().string(), 0) == 0) {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }
    if (ec) {
        error = "could not remove " + std::string(info.name) + ": " + ec.message();
        return false;
    }

    json manifest = read_manifest(manifest_file());
    manifest.erase(std::string(info.id));
    std::ofstream out(manifest_file());
    if (out) {
        out << manifest.dump(2) << '\n';
    }

    // The build tree is far larger than the module and is pure cache.
    std::filesystem::remove_all(paths::runtime_build_dir() / std::string(info.id), ec);
    return true;
}

}  // namespace crucible
