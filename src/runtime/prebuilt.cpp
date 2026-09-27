// SPDX-License-Identifier: MIT
#include "crucible/runtime/prebuilt.hpp"

#include <algorithm>
#include <system_error>

#include <nlohmann/json.hpp>

#include "crucible/util/subprocess.hpp"

#ifndef CRUCIBLE_LLAMA_TAG
#define CRUCIBLE_LLAMA_TAG "unknown"
#endif
#ifndef CRUCIBLE_VERSION
#define CRUCIBLE_VERSION "0.0.0"
#endif

namespace crucible::prebuilt {
namespace {

using json = nlohmann::json;

constexpr const char* kRepository = "mattsaund/Crucible";

/// The extension a loadable module has here. Matches registry.cpp's, and for
/// the same reason: it is how a module is told from the README beside it.
std::string_view module_suffix() {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

/// Run a command to completion, discarding its output. There is no shared
/// helper for this: everything else that shells out here wants the output.
int run_quiet(const std::vector<std::string>& argv) {
    util::Subprocess child;
    std::string      error;
    if (!child.start(argv, {}, /*extra_env=*/{}, error)) {
        return -1;
    }
    std::string line;
    while (child.read_line(line)) {
        // Drained rather than ignored: a child whose pipe fills stops.
    }
    return child.wait();
}

}  // namespace

std::string platform_tag() {
#if defined(_WIN32)
    return "windows-x64";   // the only Windows runner, and the only one shipped
#elif defined(__APPLE__)
#  if defined(__aarch64__) || defined(__arm64__)
    return "macos-arm64";
#  else
    return "macos-x64";
#  endif
#elif defined(__linux__)
#  if defined(__x86_64__)
    return "linux-x64";
#  elif defined(__aarch64__)
    return "linux-arm64";
#  else
    return {};
#  endif
#else
    return {};
#endif
}

std::string asset_name(BackendKind kind) {
    const std::string platform = platform_tag();
    if (platform.empty()) {
        return {};
    }
    return "runtime-" + std::string(backend_info(kind).id) + "-" + platform + "-"
         + CRUCIBLE_LLAMA_TAG + ".tar.gz";
}

std::string releases_url() {
    // One page is plenty: an asset older than thirty releases is one for a
    // llama.cpp this binary was not built against, which is not downloadable
    // by name anyway.
    return std::string("https://api.github.com/repos/") + kRepository
         + "/releases?per_page=30";
}

std::string find_asset(std::string_view releases_json, std::string_view name) {
    const json parsed = json::parse(releases_json, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_array()) {
        return {};   // a rate-limit note, an error, or nothing published yet
    }
    for (const json& release : parsed) {
        const auto assets = release.find("assets");
        if (assets == release.end() || !assets->is_array()) {
            continue;
        }
        for (const json& asset : *assets) {
            const auto asset_name_field = asset.find("name");
            const auto url              = asset.find("browser_download_url");
            if (asset_name_field == asset.end() || !asset_name_field->is_string() ||
                url == asset.end() || !url->is_string()) {
                continue;
            }
            if (asset_name_field->get<std::string>() == name) {
                return url->get<std::string>();
            }
        }
    }
    return {};
}

bool fetch_releases(std::string& body, std::string& error) {
    if (!util::on_path("curl")) {
        error = "curl is not installed";
        return false;
    }
    const std::vector<std::string> argv{
        "curl", "--silent", "--show-error", "--location", "--fail",
        "--max-time", "15",
        "--user-agent", std::string("Crucible/") + CRUCIBLE_VERSION,
        "--header", "Accept: application/vnd.github+json",
        releases_url(),
    };
    util::Subprocess child;
    if (!child.start(argv, {}, /*extra_env=*/{}, error)) {
        return false;
    }
    std::string line;
    while (child.read_line(line)) {
        body += line;
        body += '\n';
    }
    if (const int status = child.wait(); status != 0) {
        error = "the release list could not be read (curl exited "
              + std::to_string(status) + ")";
        return false;
    }
    return true;
}

bool install(const std::string& url, const std::filesystem::path& into,
             std::vector<std::filesystem::path>& written, std::string& error,
             const std::function<void(std::string)>& say) {
    if (!util::on_path("curl") || !util::on_path("tar")) {
        error = "curl and tar are needed to unpack a prebuilt runtime";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(into, ec);

    // Unpacked beside the runtimes rather than into them, and moved across only
    // once the whole archive is out. A tar interrupted halfway into the live
    // directory leaves half a backend behind, and half a backend is one ggml
    // will happily load.
    const std::filesystem::path staging = into.parent_path() / "runtime-download";
    const std::filesystem::path archive = into.parent_path() / "runtime-download.tar.gz";
    std::filesystem::remove_all(staging, ec);
    std::filesystem::remove(archive, ec);
    std::filesystem::create_directories(staging, ec);

    const auto clean = [&]() {
        std::error_code cleanup;
        std::filesystem::remove_all(staging, cleanup);
        std::filesystem::remove(archive, cleanup);
    };

    say("downloading");
    if (run_quiet({"curl", "--location", "--fail", "--silent", "--show-error",
                   "--max-time", "600", "--output", archive.string(), url}) != 0) {
        error = "the download failed";
        clean();
        return false;
    }

    say("unpacking");
    if (run_quiet({"tar", "-xzf", archive.string(), "-C", staging.string()}) != 0) {
        error = "the archive could not be unpacked";
        clean();
        return false;
    }

    // Everything that looks like a module, wherever the archive put it.
    std::vector<std::filesystem::path> found;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(staging, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        const std::string_view suffix = module_suffix();
        if (name.size() > suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            found.push_back(entry.path());
        }
    }
    if (found.empty()) {
        error = "the archive held no modules";
        clean();
        return false;
    }

    for (const std::filesystem::path& module : found) {
        const std::filesystem::path landing = into / module.filename();
        std::filesystem::rename(module, landing, ec);
        if (ec) {
            // Across filesystems a rename is not a move. The runtimes directory
            // and the staging directory are siblings, so this is the unusual
            // case rather than the expected one -- but it is not an error.
            ec.clear();
            std::filesystem::copy_file(
                module, landing, std::filesystem::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            error = "could not put " + module.filename().string() + " in place";
            clean();
            return false;
        }
        written.push_back(landing);
    }

    clean();
    return true;
}

bool try_install(BackendKind kind, const std::filesystem::path& into,
                 std::vector<std::filesystem::path>& written,
                 const std::function<void(std::string)>& say) {
    const std::string name = asset_name(kind);
    if (name.empty()) {
        return false;   // nothing is published for this platform
    }

    std::string body;
    std::string error;
    if (!fetch_releases(body, error)) {
        return false;
    }
    const std::string url = find_asset(body, name);
    if (url.empty()) {
        return false;   // this backend is not published, or not for this tag
    }
    if (!install(url, into, written, error, say)) {
        return false;
    }
    return true;
}

}  // namespace crucible::prebuilt
