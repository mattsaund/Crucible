// SPDX-License-Identifier: MIT
//
// See python.hpp.
#include "crucible/lab/python.hpp"

#include <algorithm>
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

// python-build-standalone's build of 2026-10-21: CPython 3.14.9. The newest
// Python that every wheel Crucible installs is published for on every
// platform: torch, sentencepiece, onnxruntime and mlx all have 3.14 wheels,
// and none of them has 3.15 ones yet.
constexpr const char* kVersion = "3.14.9";
constexpr const char* kRelease =
    "https://github.com/astral-sh/python-build-standalone/releases/download/20261021/"
    "cpython-3.14.9%2B20261021-";

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
    return make("x86_64-pc-windows-msvc", 50270697,
                "60739023ce2d9dee1dfde665c73bc61735cd8ecc5e0277345f1dc0ba6c0fc9ba");
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    return make("aarch64-apple-darwin", 26834117,
                "226d696642a8a53d03b4d5b255b1c319b1575a6d44bd723a953c9f507755e36a");
#elif defined(__APPLE__) && defined(__x86_64__)
    return make("x86_64-apple-darwin", 26931969,
                "600cf336dd007e7c72318214fd0708a56c555b516964175df4a748e4ff0487a2");
#elif defined(__linux__) && defined(__x86_64__)
    return make("x86_64-unknown-linux-gnu", 76270309,
                "dfa4c5a40c47f338f41658017be550c72247c391e6bfb1fa4da03ebc8451abdf");
#elif defined(__linux__) && defined(__aarch64__)
    return make("aarch64-unknown-linux-gnu", 58711403,
                "847895ba727440b012207bb0192282d7e182eb991ae5c7bb5c35c6a12a99e732");
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

namespace {

/// "3.12.15" or "3.12.15.final.0" -> "3.12". Empty for anything else.
std::string minor_version(const std::string& version) {
    const std::size_t first = version.find('.');
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t second = version.find('.', first + 1);
    return version.substr(0, second);
}

/// `inner` is `outer` or inside it, both made canonical first.
bool inside(const std::filesystem::path& inner, const std::filesystem::path& outer) {
    std::error_code ec;
    const std::filesystem::path a = std::filesystem::weakly_canonical(inner, ec);
    const std::filesystem::path b = std::filesystem::weakly_canonical(outer, ec);
    const auto mismatch = std::mismatch(b.begin(), b.end(), a.begin(), a.end());
    return !b.empty() && mismatch.first == b.end();
}

}  // namespace

bool made_from_another_python(const std::filesystem::path& venv) {
    if (const char* chosen = std::getenv("CRUCIBLE_PYTHON"); chosen != nullptr && *chosen != '\0') {
        return false;
    }
    const std::optional<Build> build = build_here();
    std::ifstream in(venv / "pyvenv.cfg");
    if (!build || !in) {
        return false;
    }
    // pyvenv.cfg is "key = value" lines: `home` is the folder of the
    // interpreter it was made from, and `version` (or `version_info`, as
    // virtualenv writes it) that interpreter's version.
    std::string home;
    std::string version;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        std::string key   = line.substr(0, equals);
        std::string value = line.substr(equals + 1);
        const auto trim = [](std::string& text) {
            while (!text.empty() && (text.back() == ' ' || text.back() == '\r' || text.back() == '\t')) {
                text.pop_back();
            }
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
                text.erase(0, 1);
            }
        };
        trim(key);
        trim(value);
        if (key == "home") {
            home = value;
        } else if (key == "version" || (key == "version_info" && version.empty())) {
            version = value;
        }
    }
    if (home.empty() || version.empty() || !inside(home, root())) {
        return false;
    }
    return minor_version(version) != minor_version(build->version);
}

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
