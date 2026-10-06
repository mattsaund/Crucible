// SPDX-License-Identifier: MIT
//
// See python.hpp.
#include "crucible/lab/python.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "crucible/config/paths.hpp"
#include "crucible/util/http.hpp"
#include "crucible/util/sha256.hpp"
#include "crucible/util/subprocess.hpp"

namespace crucible::lab::python {
namespace {

using json = nlohmann::json;

// python-build-standalone's build of 2026-10-03: CPython 3.12.15. 3.12 rather
// than the newest because it is the version every wheel the trainer installs
// -- torch, bitsandbytes, mlx -- is published for on every platform.
constexpr const char* kVersion = "3.12.15";
constexpr const char* kRelease =
    "https://github.com/astral-sh/python-build-standalone/releases/download/20261003/"
    "cpython-3.12.15%2B20261003-";

std::atomic<int> g_installing{0};

int run_quiet(const std::vector<std::string>& argv) {
    util::Subprocess child;
    std::string      error;
    if (!child.start(argv, {}, {}, error)) {
        return -1;
    }
    std::string line;
    while (child.read_line(line)) {
        // Drained rather than ignored: a child whose pipe fills stops.
    }
    return child.wait();
}

}  // namespace

std::optional<Build> build_here() {
    const auto make = [](const char* triple, std::uint64_t bytes, const char* sha256) {
        return Build{kVersion, std::string(kRelease) + triple + "-install_only.tar.gz", bytes, sha256};
    };
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return make("x86_64-pc-windows-msvc", 46509797,
                "4b6f0beebbb695a0f3ea237b8c3eaa5bd424f47a7bc25b2fbe3a43390c770f08");
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    return make("aarch64-apple-darwin", 25146462,
                "316a463172740e71d8dca1f2730784e325f3f720941137b5d674d5801a632213");
#elif defined(__APPLE__) && defined(__x86_64__)
    return make("x86_64-apple-darwin", 24850741,
                "a8fd7a91852f19b6d959793ef41fad048631ccb2a334a9ecdf573255298f7978");
#elif defined(__linux__) && defined(__x86_64__)
    return make("x86_64-unknown-linux-gnu", 66924371,
                "f937814031eab4698ca6d07ec606ede1825768f3f3e99af76d9db3900bee03c5");
#elif defined(__linux__) && defined(__aarch64__)
    return make("aarch64-unknown-linux-gnu", 52330698,
                "95c01982c9fcb9d95b0acfdb5eb8a6e0099dd11edf062314a474228d93f2b765");
#else
    return std::nullopt;
#endif
}

std::filesystem::path root() { return paths::data_dir() / "python"; }

std::filesystem::path marker_file() { return root() / "crucible-python.json"; }

std::filesystem::path interpreter() {
    if (const char* chosen = std::getenv("CRUCIBLE_PYTHON"); chosen != nullptr && *chosen != '\0') {
        return chosen;
    }
#if defined(_WIN32)
    return root() / "python.exe";
#else
    return root() / "bin" / "python3";
#endif
}

std::string installed_version() {
    std::ifstream in(marker_file());
    if (!in) {
        return {};
    }
    try {
        json doc;
        in >> doc;
        return doc.value("version", "");
    } catch (const json::exception&) {
        return {};
    }
}

bool installed() {
    std::error_code ec;
    if (const char* chosen = std::getenv("CRUCIBLE_PYTHON"); chosen != nullptr && *chosen != '\0') {
        return true;   // somebody else's to keep, and theirs to have got right
    }
    const std::optional<Build> build = build_here();
    return build && std::filesystem::exists(interpreter(), ec)
        && installed_version() == build->version;
}

bool installing() { return g_installing.load() > 0; }

bool install(const std::function<void(const Progress&)>& on_progress, std::string& error) {
    struct Busy {
        Busy() { ++g_installing; }
        ~Busy() { --g_installing; }
    } busy;
    const std::optional<Build> build = build_here();
    if (!build) {
        error = "no build of Python is published for this platform";
        return false;
    }
    if (!util::on_path("tar")) {
        error = "tar is needed to unpack Python";
        return false;
    }
    std::error_code ec;
    const std::filesystem::path data = paths::data_dir();
    std::filesystem::create_directories(data, ec);
    const std::filesystem::path archive = data / "python-download.tar.gz";
    const std::filesystem::path staging = data / "python-staging";
    std::filesystem::remove(archive, ec);

    // --- the download, measured by how much of the file has arrived ---------
    std::atomic<bool> finished{false};
    bool              fetched = false;
    std::string       why;
    std::thread fetch([&] {
        fetched = util::http::download(build->url, archive, 1800, why);
        finished.store(true);
    });
    while (!finished.load()) {
        if (on_progress) {
            std::error_code size_ec;
            const std::uintmax_t got = std::filesystem::file_size(archive, size_ec);
            on_progress(Progress{"downloading", size_ec ? 0 : static_cast<std::uint64_t>(got),
                                 build->bytes});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    fetch.join();
    if (!fetched) {
        std::filesystem::remove(archive, ec);
        error = "Python could not be downloaded: " + why;
        return false;
    }

    // --- is it the file it should be ----------------------------------------
    if (on_progress) {
        on_progress(Progress{"checking", build->bytes, build->bytes});
    }
    const std::string digest = util::sha256_file(archive);
    if (digest != build->sha256) {
        std::filesystem::remove(archive, ec);
        error = "the Python download is not the file it should be (its SHA-256 is "
              + (digest.empty() ? std::string("unreadable") : digest) + ")";
        return false;
    }

    // --- unpacked beside the old one, then swapped in -----------------------
    //
    // A tar interrupted halfway into the live folder leaves an interpreter
    // that starts and then cannot import half its standard library.
    if (on_progress) {
        on_progress(Progress{"unpacking", build->bytes, build->bytes});
    }
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);
    if (run_quiet({"tar", "-xzf", archive.string(), "-C", staging.string()}) != 0
        || !std::filesystem::exists(staging / "python", ec)) {
        std::filesystem::remove_all(staging, ec);
        std::filesystem::remove(archive, ec);
        error = "the Python download could not be unpacked";
        return false;
    }
    std::filesystem::remove_all(root(), ec);
    std::filesystem::rename(staging / "python", root(), ec);
    std::filesystem::remove_all(staging, ec);
    std::filesystem::remove(archive, ec);
    if (ec || !std::filesystem::exists(interpreter())) {
        error = "Python could not be put in place at " + root().string();
        return false;
    }

    // Last, so a marker means everything before it happened.
    std::ofstream marker(marker_file());
    marker << json{{"version", build->version}, {"sha256", build->sha256}}.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
    if (!marker) {
        error = "could not write " + marker_file().string();
        return false;
    }
    return true;
}

}  // namespace crucible::lab::python
