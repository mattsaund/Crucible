// SPDX-License-Identifier: MIT
#include "crucible/runtime/cuda_libraries.hpp"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <system_error>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

#include "crucible/config/paths.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/http.hpp"
#include "crucible/util/subprocess.hpp"

namespace crucible::cuda_libraries {
namespace {

using json = nlohmann::json;

/// Where NVIDIA publishes its redistributable components, and the manifest
/// that lists them for one release.
constexpr std::string_view kRedist = "https://developer.download.nvidia.com/compute/cuda/redist/";

/// NVIDIA's name for this platform in that manifest, or empty.
std::string platform_key() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return "windows-x86_64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x86_64";
#else
    return {};
#endif
}

/// Which manifest entry holds which file. The runtime is its own component;
/// cuBLAS ships its implementation library in the same archive as itself.
struct Component {
    const char*                     key;
    std::vector<std::string_view>   files;
};

std::vector<Component> components() {
#if defined(_WIN32)
    return {{"cuda_cudart", {"cudart64_13.dll"}},
            {"libcublas",   {"cublasLt64_13.dll", "cublas64_13.dll"}}};
#else
    return {{"cuda_cudart", {"libcudart.so.13"}},
            {"libcublas",   {"libcublasLt.so.13", "libcublas.so.13"}}};
#endif
}

/// Run to completion, discarding the output -- which still has to be read, or
/// a child whose pipe fills stops.
int run_quiet(const std::vector<std::string>& argv) {
    util::Subprocess child;
    std::string      error;
    if (!child.start(argv, {}, {}, error)) {
        return -1;
    }
    std::string line;
    while (child.read_line(line)) {
    }
    return child.wait();
}

/// Is `name` a version of `wanted`? "libcudart.so.13.3.29" is the real file
/// behind "libcudart.so.13", which is a link to it; on Windows the name is
/// already the file.
bool is_version_of(const std::string& name, std::string_view wanted) {
    return name == wanted
        || (name.size() > wanted.size() && name.compare(0, wanted.size(), wanted) == 0
            && name[wanted.size()] == '.');
}

std::mutex g_preload_mutex;
std::vector<std::string> g_preloaded;

}  // namespace

std::filesystem::path dir() {
    return paths::runtimes_dir() / "cuda";
}

std::vector<std::string> wanted() {
    if (platform_key().empty()) {
        return {};
    }
    // Load order: the runtime first, then cuBLAS's implementation, then the
    // library that needs it.
#if defined(_WIN32)
    return {"cudart64_13.dll", "cublasLt64_13.dll", "cublas64_13.dll"};
#else
    return {"libcudart.so.13", "libcublasLt.so.13", "libcublas.so.13"};
#endif
}

bool complete() {
    const std::vector<std::string> names = wanted();
    if (names.empty()) {
        return false;
    }
    std::error_code ec;
    return std::all_of(names.begin(), names.end(), [&ec](const std::string& name) {
        return std::filesystem::is_regular_file(dir() / name, ec);
    });
}

bool on_system() {
    const std::vector<std::string> names = wanted();
    if (names.empty()) {
        return false;
    }
    for (const std::string& name : names) {
        // By name alone, so the system's own search path is what is asked.
#if defined(_WIN32)
        HMODULE handle = LoadLibraryA(name.c_str());
        if (handle == nullptr) {
            return false;
        }
        FreeLibrary(handle);
#else
        void* handle = dlopen(name.c_str(), RTLD_LAZY | RTLD_LOCAL);
        if (handle == nullptr) {
            return false;
        }
        dlclose(handle);
#endif
    }
    return true;
}

namespace detail {

std::vector<Archive> archives(std::string_view manifest, std::string_view platform) {
    const json parsed = json::parse(manifest, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_object()) {
        return {};
    }
    std::vector<Archive> out;
    for (const Component& component : components()) {
        const auto entry = parsed.find(component.key);
        if (entry == parsed.end() || !entry->is_object()) {
            return {};
        }
        const auto build = entry->find(std::string(platform));
        if (build == entry->end() || !build->is_object()) {
            return {};
        }
        const std::string relative = build->value("relative_path", "");
        if (relative.empty()) {
            return {};
        }
        Archive archive;
        archive.url = std::string(kRedist) + relative;
        // NVIDIA writes the size as a string -- "817981368" -- and a reader
        // that asked for a number threw, which on the thread installing the
        // runtime took the whole program down.
        if (const auto size = build->find("size"); size != build->end()) {
            if (size->is_number_unsigned()) {
                archive.size = size->get<std::uint64_t>();
            } else if (size->is_string()) {
                archive.size = std::strtoull(size->get<std::string>().c_str(), nullptr, 10);
            }
        }
        for (const std::string_view file : component.files) {
            archive.files.emplace_back(file);
        }
        out.push_back(std::move(archive));
    }
    return out;
}

}  // namespace detail

bool fetch(std::string& error, const std::function<void(std::string)>& say) {
    const std::string platform = platform_key();
    if (platform.empty()) {
        error = "no CUDA libraries are published for this platform";
        return false;
    }
    const auto tell = [&say](std::string what) {
        if (say) {
            say(std::move(what));
        }
    };
    if (!util::http::available(error)) {
        return false;
    }
    if (!util::on_path("tar")) {
        error = "tar is needed to unpack the CUDA libraries";
        return false;
    }
#if !defined(_WIN32)
    if (!util::on_path("xz")) {
        error = "xz is needed to unpack the CUDA libraries -- install the xz-utils package";
        return false;
    }
#endif

    tell("asking NVIDIA for the CUDA libraries");
    util::http::Request request;
    request.url = std::string(kRedist) + "redistrib_" + std::string(kRelease) + ".json";
    request.timeout_seconds = 30;
    const util::http::Response manifest = util::http::send(request);
    if (!manifest.ok()) {
        error = "NVIDIA's list of CUDA " + std::string(kRelease) + " libraries could not be read ("
              + manifest.reason() + ")";
        return false;
    }
    const std::vector<detail::Archive> archives = detail::archives(manifest.body, platform);
    if (archives.empty()) {
        error = "NVIDIA's list for CUDA " + std::string(kRelease) + " does not have them for "
              + platform;
        return false;
    }

    // Staged beside the runtimes and moved in only when every file is out,
    // for the reason the modules are: half a set loads, and then fails later
    // in a way that says nothing about why.
    std::error_code ec;
    const std::filesystem::path staging = paths::runtimes_dir().parent_path() / "cuda-download";
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);
    const auto clean = [&staging]() {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
    };

    std::uint64_t total = 0;
    for (const detail::Archive& archive : archives) {
        total += archive.size;
    }
    std::uint64_t done = 0;
    bool unpacked_cleanly = true;
    for (std::size_t i = 0; i < archives.size(); ++i) {
        const detail::Archive& archive = archives[i];
        const std::string file_name = archive.url.substr(archive.url.rfind('/') + 1);
        const std::filesystem::path local = staging / file_name;

        tell("downloading NVIDIA's CUDA libraries, " + format::bytes(done) + " of "
             + format::bytes(total));
        // An hour: cuBLAS is most of a gigabyte, and a timeout halfway through
        // reads as "it does not work" rather than "the line is slow".
        if (std::string why; !util::http::download(archive.url, local, 3600, why)) {
            error = "the CUDA libraries could not be downloaded: " + why;
            clean();
            return false;
        }
        if (archive.size != 0 && std::filesystem::file_size(local, ec) != archive.size) {
            error = file_name + " arrived the wrong size -- the download was cut short";
            clean();
            return false;
        }
        done += archive.size;

        tell("unpacking " + file_name);
        const std::filesystem::path out = staging / ("unpacked-" + std::to_string(i));
        std::filesystem::create_directories(out, ec);
#if defined(_WIN32)
        // Windows' own tar reads a zip.
        const int status = run_quiet({"tar", "-xf", local.string(), "-C", out.string()});
#else
        // Only the shared libraries: the archive is mostly static libraries
        // and headers, a gigabyte of disk nobody here will read.
        std::vector<std::string> argv{"tar", "-xJf", local.string(), "-C", out.string(),
                                      "--wildcards"};
        for (const std::string& want : archive.files) {
            argv.push_back("*/" + want.substr(0, want.find(".so")) + ".so*");
        }
        const int status = run_quiet(argv);
#endif
        std::filesystem::remove(local, ec);
        // Not a failure on its own. On a filesystem that cannot hold a link,
        // tar complains about the two links beside each library and still
        // writes the library -- which is the only file wanted. Whether that
        // happened is asked below, by looking.
        unpacked_cleanly = unpacked_cleanly && status == 0;
    }

    // Each wanted name, as a plain file. On Linux the name is a link to a link
    // to the versioned library; the links are followed here rather than kept,
    // so the folder works on a filesystem that cannot hold one.
    const std::filesystem::path landing = dir();
    std::filesystem::create_directories(landing, ec);
    for (const std::string& want : wanted()) {
        std::filesystem::path real;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::recursive_directory_iterator(staging, ec)) {
            const std::string name = entry.path().filename().string();
            if (!is_version_of(name, want)) {
                continue;
            }
            std::error_code check;
            const std::filesystem::path resolved = std::filesystem::canonical(entry.path(), check);
            if (!check && std::filesystem::is_regular_file(resolved, check)) {
                real = resolved;
                break;
            }
        }
        if (real.empty()) {
            error = want + (unpacked_cleanly ? " was not in NVIDIA's archive"
                                             : " could not be unpacked from NVIDIA's archive");
            clean();
            return false;
        }
        std::filesystem::copy_file(real, landing / want,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "could not put " + want + " in place: " + ec.message();
            clean();
            return false;
        }
    }
    clean();
    tell("CUDA libraries in place");
    return true;
}

void preload() {
    if (!complete()) {
        return;
    }
    const std::lock_guard<std::mutex> lock(g_preload_mutex);
    for (const std::string& name : wanted()) {
        if (std::find(g_preloaded.begin(), g_preloaded.end(), name) != g_preloaded.end()) {
            continue;
        }
        const std::filesystem::path path = dir() / name;
        // By full path, and never closed. Once one of these is loaded, a CUDA
        // module that names it as a dependency is given the loaded copy: the
        // loader matches what is already in memory before it searches. That is
        // what makes the module's own search path -- which names a folder on
        // the machine that built it -- not matter.
#if defined(_WIN32)
        const bool loaded = LoadLibraryExW(path.wstring().c_str(), nullptr,
                                           LOAD_WITH_ALTERED_SEARCH_PATH) != nullptr;
#else
        const bool loaded = dlopen(path.string().c_str(), RTLD_NOW | RTLD_GLOBAL) != nullptr;
#endif
        if (loaded) {
            g_preloaded.push_back(name);
        }
    }
}

std::uintmax_t bytes() {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (const std::string& name : wanted()) {
        const std::uintmax_t size = std::filesystem::file_size(dir() / name, ec);
        total += ec ? 0 : size;
    }
    return total;
}

void remove() {
    std::error_code ec;
    std::filesystem::remove_all(dir(), ec);
}

}  // namespace crucible::cuda_libraries
