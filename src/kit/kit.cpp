// SPDX-License-Identifier: MIT
//
// See kit.hpp.
#include "crucible/kit/kit.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <thread>

#include <nlohmann/json.hpp>

#include "crucible/config/paths.hpp"
#include "crucible/lab/python.hpp"
#include "crucible/tools/fetch.hpp"
#include "crucible/util/http.hpp"
#include "crucible/util/subprocess.hpp"

namespace crucible::kit {
namespace {

namespace fs = std::filesystem;
using json   = nlohmann::json;

#if defined(_WIN32)
constexpr char kPathSeparator = ';';
constexpr const char* kExe    = ".exe";
#else
constexpr char kPathSeparator = ':';
constexpr const char* kExe    = "";
#endif

/// The marker an install leaves when it has finished: a piece half unpacked
/// by a Crucible closed mid-way is not a piece.
fs::path marker(const std::string& id) { return dir() / id / ".crucible-kit"; }

/// A JSON listing, asked for over HTTPS.
json listing(const std::string& url, std::string& error) {
    util::http::Request request;
    request.url             = url;
    request.timeout_seconds = 30;
    request.headers         = {{"Accept", "application/json"}, {"User-Agent", "Crucible"}};
    const util::http::Response response = util::http::send(request);
    if (!response.ok()) {
        error = url + ": " + response.reason();
        return json();
    }
    json parsed = json::parse(response.body, nullptr, false);
    if (parsed.is_discarded()) {
        error = url + " did not answer with JSON";
        return json();
    }
    return parsed;
}

/// Run a program to its end. Its output, when it failed, in `said`.
bool run(const std::vector<std::string>& argv, std::string& said, const fs::path& cwd = {}) {
    util::Subprocess child;
    std::string error;
    if (!child.start(argv, cwd, {}, error)) {
        said = error;
        return false;
    }
    std::string line;
    std::string all;
    while (child.read_line(line)) {
        all += line + "\n";
        if (all.size() > 8000) {
            all.erase(0, all.size() - 8000);
        }
    }
    const bool ok = child.wait() == 0;
    if (!ok) {
        said = all;
    }
    return ok;
}

/// Fetch `url` to `to`, telling `progress` how much has arrived of `total`.
bool download(const std::string& url, const fs::path& to, std::uint64_t total,
              const std::function<void(std::uint64_t, std::uint64_t)>& progress,
              const std::function<bool()>& cancel, std::string& error) {
    std::error_code ec;
    fs::remove(to, ec);
    std::atomic<bool> finished{false};
    bool              fetched = false;
    std::string       why;
    std::thread fetch([&] {
        fetched = util::http::download(url, to, 1800, why);
        finished.store(true);
    });
    while (!finished.load()) {
        if (progress) {
            std::error_code size_ec;
            const std::uintmax_t got = fs::file_size(to, size_ec);
            progress(size_ec ? 0 : static_cast<std::uint64_t>(got), total);
        }
        if (cancel && cancel()) {
            // The download finishes in the background and is thrown away;
            // http has no way to stop one request alone.
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    fetch.join();
    if (!fetched) {
        fs::remove(to, ec);
        error = "the download failed: " + why;
        return false;
    }
    return true;
}

/// Unpack `archive` into `into`, by whichever means this machine has. A zip
/// is bsdtar's to open on a Mac and on Windows, which ship it; on Linux,
/// unzip's when it is there and Crucible's own Python's when it is not.
bool unpack(const fs::path& archive, const fs::path& into, std::string& error) {
    std::error_code ec;
    fs::remove_all(into, ec);
    fs::create_directories(into, ec);
    const std::string name = archive.filename().string();
    const auto ends = [&name](const std::string& tail) {
        return name.size() >= tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0;
    };
    std::string said;
    bool ok = false;
    if (ends(".zip")) {
#if defined(__linux__)
        if (util::on_path("unzip")) {
            ok = run({"unzip", "-q", "-o", archive.string(), "-d", into.string()}, said);
        } else if (lab::python::installed()) {
            ok = run({lab::python::interpreter().string(), "-I", "-m", "zipfile", "-e", archive.string(),
                      into.string()}, said);
        } else {
            said = "there is no unzip, and Crucible's Python is not installed yet";
        }
#else
        ok = run({"tar", "-xf", archive.string(), "-C", into.string()}, said);
#endif
    } else if (ends(".tar.xz")) {
        ok = run({"tar", "-xJf", archive.string(), "-C", into.string()}, said);
    } else {
        ok = run({"tar", "-xzf", archive.string(), "-C", into.string()}, said);
    }
    if (!ok) {
        error = "could not unpack " + name + (said.empty() ? std::string() : ": " + said);
    }
    return ok;
}

/// What an archive unpacked to: its one top folder, or the folder itself
/// when it has several things at the top, as MinGit does.
fs::path top_of(const fs::path& staging) {
    std::error_code ec;
    std::vector<fs::path> entries;
    for (const auto& entry : fs::directory_iterator(staging, ec)) {
        entries.push_back(entry.path());
    }
    if (entries.size() == 1 && fs::is_directory(entries.front(), ec)) {
        return entries.front();
    }
    return staging;
}

/// Put what was unpacked in its place, and mark it finished.
bool settle(const std::string& id, const fs::path& staging, std::string& error) {
    std::error_code ec;
    const fs::path place = dir() / id;
    fs::remove_all(place, ec);
    fs::rename(top_of(staging), place, ec);
    if (ec) {
        error = "could not put " + id + " in place: " + ec.message();
        return false;
    }
    fs::remove_all(staging, ec);
    std::ofstream(marker(id)) << "installed by Crucible\n";
    return true;
}

/// Make a program runnable, for an unpacker that does not keep the bit.
void runnable(const fs::path& file) {
    std::error_code ec;
    fs::permissions(file, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
}

/// One piece: its listing read, its download fetched, unpacked and put in place.
bool fetch_piece(const std::string& id, const std::string& url, std::uint64_t bytes, const std::string& extension,
                 const std::function<void(std::uint64_t, std::uint64_t)>& progress,
                 const std::function<bool()>& cancel, std::string& error) {
    std::error_code ec;
    fs::create_directories(dir(), ec);
    const fs::path archive = dir() / (id + "-download" + extension);
    const fs::path staging = dir() / (id + "-staging");
    if (!download(url, archive, bytes, progress, cancel, error)) {
        return false;
    }
    if (cancel && cancel()) {
        fs::remove(archive, ec);
        error = "stopped";
        return false;
    }
    const bool ok = unpack(archive, staging, error) && settle(id, staging, error);
    fs::remove(archive, ec);
    return ok;
}

std::string extension_of(const std::string& url) {
    for (const char* known : {".tar.gz", ".tar.xz", ".zip", ".tgz"}) {
        const std::string tail(known);
        if (url.size() >= tail.size() && url.compare(url.size() - tail.size(), tail.size(), tail) == 0) {
            return tail;
        }
    }
    return ".tar.gz";
}

}  // namespace

namespace detail {

Platform platform_here() {
    Platform here;
#if defined(_WIN32)
    here.os = "windows";
#elif defined(__APPLE__)
    here.os = "macos";
#else
    here.os = "linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    here.arch = "arm64";
#else
    here.arch = "x64";
#endif
    return here;
}

std::string gh_asset(std::string_view release_json, const Platform& here, std::uint64_t& bytes) {
    const json release = json::parse(release_json, nullptr, false);
    if (!release.is_object() || !release.contains("assets")) {
        return {};
    }
    const std::string arch = here.arch == "arm64" ? "arm64" : "amd64";
    const std::string tail = here.os == "macos"   ? "_macOS_" + arch + ".zip"
                           : here.os == "windows" ? "_windows_" + arch + ".zip"
                                                  : "_linux_" + arch + ".tar.gz";
    for (const json& asset : release["assets"]) {
        const std::string name = asset.value("name", "");
        if (name.size() > tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0) {
            bytes = asset.value("size", std::uint64_t{0});
            return asset.value("browser_download_url", "");
        }
    }
    return {};
}

std::string node_url(std::string_view index_json, const Platform& here, std::string& version) {
    const json index = json::parse(index_json, nullptr, false);
    if (!index.is_array()) {
        return {};
    }
    for (const json& release : index) {
        // The first long-term-support release in the index is the newest one.
        if (!release.contains("lts") || release["lts"].is_boolean()) {
            continue;
        }
        version = release.value("version", "");
        if (version.empty()) {
            continue;
        }
        const std::string os   = here.os == "macos" ? "darwin" : here.os == "windows" ? "win" : "linux";
        const std::string ext  = here.os == "windows" ? ".zip" : here.os == "macos" ? ".tar.gz" : ".tar.xz";
        return "https://nodejs.org/dist/" + version + "/node-" + version + "-" + os + "-" + here.arch + ext;
    }
    return {};
}

std::string browser_url(std::string_view listing_json, const Platform& here) {
    const json listing_doc = json::parse(listing_json, nullptr, false);
    if (!listing_doc.is_object()) {
        return {};
    }
    const std::string wanted = here.os == "macos"   ? (here.arch == "arm64" ? "mac-arm64" : "mac-x64")
                             : here.os == "windows" ? "win64"
                                                    : (here.arch == "arm64" ? "linux-arm64" : "linux64");
    const json downloads = listing_doc.value("channels", json::object())
                                      .value("Stable", json::object())
                                      .value("downloads", json::object())
                                      .value("chrome-headless-shell", json::array());
    for (const json& one : downloads) {
        if (one.value("platform", "") == wanted) {
            return one.value("url", "");
        }
    }
    return {};
}

std::string mingit_asset(std::string_view release_json, const Platform& here, std::uint64_t& bytes) {
    const json release = json::parse(release_json, nullptr, false);
    if (!release.is_object() || !release.contains("assets")) {
        return {};
    }
    const std::string tail = here.arch == "arm64" ? "-arm64.zip" : "-64-bit.zip";
    for (const json& asset : release["assets"]) {
        const std::string name = asset.value("name", "");
        if (name.rfind("MinGit-", 0) == 0 && name.find("busybox") == std::string::npos
            && name.size() > tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0) {
            bytes = asset.value("size", std::uint64_t{0});
            return asset.value("browser_download_url", "");
        }
    }
    return {};
}

}  // namespace detail

std::vector<Piece> pieces() {
    return {
        {"gh", "GitHub's command line", "publishing to GitHub, pull requests and releases", 15U << 20},
        {"node", "Node.js", "running and testing JavaScript projects", 50U << 20},
        {"browser", "A headless browser", "reading pages drawn by script, and pictures of pages", 100U << 20},
        {"git", "Git", "version control", 40U << 20},
        {"ocr", "Reading text off pictures", "for models that read text only", 90U << 20},
    };
}

fs::path dir() { return paths::data_dir() / "kit"; }

bool installed(const std::string& id) {
    std::error_code ec;
    return fs::exists(marker(id), ec);
}

std::vector<Piece> missing() {
    const detail::Platform here = detail::platform_here();
    std::vector<Piece> out;
    for (const Piece& piece : pieces()) {
        if (installed(piece.id)) {
            continue;
        }
        bool wanted = false;
        if (piece.id == "gh") {
            wanted = !util::on_path("gh");
        } else if (piece.id == "node") {
            wanted = !util::on_path("node");
        } else if (piece.id == "browser") {
            wanted = tools::system_browser().empty();
        } else if (piece.id == "git") {
            wanted = here.os == "windows" && !util::on_path("git");
        } else if (piece.id == "ocr") {
            // A Mac reads text with its own Vision framework and Windows with
            // its own OCR engine; Linux has neither built in.
            wanted = here.os == "linux" && !util::on_path("tesseract");
        }
        if (wanted) {
            out.push_back(piece);
        }
    }
    return out;
}

std::vector<fs::path> bin_dirs() {
    std::vector<fs::path> out;
    if (installed("gh")) {
        out.push_back(dir() / "gh" / "bin");
    }
    if (installed("node")) {
#if defined(_WIN32)
        out.push_back(dir() / "node");
#else
        out.push_back(dir() / "node" / "bin");
#endif
    }
    if (installed("git")) {
        out.push_back(dir() / "git" / "cmd");
    }
    return out;
}

void put_on_path() {
    const char* now = std::getenv("PATH");
    std::string path = now != nullptr ? now : "";
    std::string front;
    for (const fs::path& folder : bin_dirs()) {
        const std::string entry = folder.string();
        // Already there, from a call before this one: not added twice.
        const std::string padded = std::string(1, kPathSeparator) + path + std::string(1, kPathSeparator);
        if (padded.find(std::string(1, kPathSeparator) + entry + std::string(1, kPathSeparator)) != std::string::npos) {
            continue;
        }
        front += entry;
        front += kPathSeparator;
    }
    if (front.empty()) {
        return;
    }
    path = front + path;
#if defined(_WIN32)
    _putenv_s("PATH", path.c_str());
#else
    setenv("PATH", path.c_str(), 1);
#endif
}

fs::path browser() {
    if (!installed("browser")) {
        return {};
    }
    const fs::path shell = dir() / "browser" / (std::string("chrome-headless-shell") + kExe);
    std::error_code ec;
    return fs::exists(shell, ec) ? shell : fs::path();
}

fs::path ocr_python() {
    if (!installed("ocr")) {
        return {};
    }
#if defined(_WIN32)
    return dir() / "ocr" / "Scripts" / "python.exe";
#else
    return dir() / "ocr" / "bin" / "python";
#endif
}

bool install(const std::string& id, const std::function<void(std::uint64_t, std::uint64_t)>& progress,
             const std::function<bool()>& cancel, std::string& error) {
    const detail::Platform here = detail::platform_here();
    bool ok = false;
    if (id == "gh") {
        const json release = listing("https://api.github.com/repos/cli/cli/releases/latest", error);
        std::uint64_t bytes = 0;
        const std::string url = release.is_null() ? std::string() : detail::gh_asset(release.dump(), here, bytes);
        if (url.empty()) {
            error = error.empty() ? "no GitHub CLI download for this machine" : error;
            return false;
        }
        ok = fetch_piece(id, url, bytes, extension_of(url), progress, cancel, error);
    } else if (id == "node") {
        const json index = listing("https://nodejs.org/dist/index.json", error);
        std::string version;
        const std::string url = index.is_null() ? std::string() : detail::node_url(index.dump(), here, version);
        if (url.empty()) {
            error = error.empty() ? "no Node.js download for this machine" : error;
            return false;
        }
        ok = fetch_piece(id, url, 50U << 20, extension_of(url), progress, cancel, error);
    } else if (id == "browser") {
        const json known = listing(
            "https://googlechromelabs.github.io/chrome-for-testing/last-known-good-versions-with-downloads.json",
            error);
        const std::string url = known.is_null() ? std::string() : detail::browser_url(known.dump(), here);
        if (url.empty()) {
            error = error.empty() ? "no headless browser download for this machine" : error;
            return false;
        }
        ok = fetch_piece(id, url, 100U << 20, ".zip", progress, cancel, error);
        if (ok) {
            runnable(browser());
        }
    } else if (id == "git") {
        const json release = listing("https://api.github.com/repos/git-for-windows/git/releases/latest", error);
        std::uint64_t bytes = 0;
        const std::string url = release.is_null() ? std::string() : detail::mingit_asset(release.dump(), here, bytes);
        if (url.empty()) {
            error = error.empty() ? "no MinGit download for this machine" : error;
            return false;
        }
        ok = fetch_piece(id, url, bytes, ".zip", progress, cancel, error);
    } else if (id == "ocr") {
        // A Python of its own, with RapidOCR -- the text detector and reader
        // as ONNX models, run by onnxruntime -- installed into it by pip.
        if (!lab::python::installed()) {
            error = "Crucible's Python is not installed yet";
            return false;
        }
        std::error_code ec;
        const fs::path place = dir() / "ocr";
        fs::remove_all(place, ec);
        fs::create_directories(dir(), ec);
        std::string said;
        if (!run({lab::python::interpreter().string(), "-m", "venv", place.string()}, said)) {
            error = "could not make a Python for reading pictures: " + said;
            return false;
        }
        if (progress) {
            progress(0, 0);
        }
#if defined(_WIN32)
        const fs::path python = place / "Scripts" / "python.exe";
#else
        const fs::path python = place / "bin" / "python";
#endif
        if (!run({python.string(), "-m", "pip", "install", "--no-cache-dir", "--disable-pip-version-check",
                  "rapidocr_onnxruntime"}, said)) {
            error = "could not install the text reader: " + said;
            return false;
        }
        std::ofstream(marker("ocr")) << "installed by Crucible\n";
        ok = true;
    } else {
        error = "no such piece: " + id;
        return false;
    }
    if (ok) {
        put_on_path();
    }
    return ok;
}

}  // namespace crucible::kit
