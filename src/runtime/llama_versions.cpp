// SPDX-License-Identifier: MIT
//
// See llama_versions.hpp.
#include "crucible/runtime/llama_versions.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <system_error>
#include <thread>

#include <nlohmann/json.hpp>

#include "crucible/config/paths.hpp"
#include "crucible/runtime/backend.hpp"
#include "crucible/runtime/prebuilt.hpp"
#include "crucible/runtime/registry.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/http.hpp"
#include "crucible/util/subprocess.hpp"

namespace crucible::llama_versions {
namespace {

using json = nlohmann::json;

/// The first line of `file`, trimmed, or empty.
std::string first_line(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::string   line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\n')) {
        line.pop_back();
    }
    return line;
}

/// The tag of the runtime the installer brought: the file beside it says,
/// and the launcher says where that is. The running one when it is that one.
std::string bundled_tag() {
    if (const char* dir = std::getenv("CRUCIBLE_BUNDLED_RUNTIME"); dir != nullptr && *dir != '\0') {
        const std::string tag = first_line(std::filesystem::path(dir) / "llama-tag");
        if (is_tag(tag)) {
            return tag;
        }
    }
    return running_tag();
}

/// The release assets of this Crucible's runtimes in `releases_json`, by tag.
std::map<std::string, std::pair<std::string, std::uint64_t>, std::less<>> published(std::string_view releases_json) {
    std::map<std::string, std::pair<std::string, std::uint64_t>, std::less<>> out;
    const json parsed = json::parse(releases_json, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_array()) {
        return out;
    }
    const std::string prefix = "runtime-core-" + prebuilt::platform_tag() + "-" + CRUCIBLE_VERSION + "-";
    const std::string suffix = ".tar.gz";
    for (const json& release : parsed) {
        if (!release.is_object() || release.value("draft", false)) {
            continue;
        }
        const auto assets = release.find("assets");
        if (assets == release.end() || !assets->is_array()) {
            continue;
        }
        for (const json& asset : *assets) {
            const std::string name = asset.value("name", std::string());
            if (name.size() <= prefix.size() + suffix.size() || name.compare(0, prefix.size(), prefix) != 0
                || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
                continue;
            }
            const std::string tag = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
            if (is_tag(tag) && out.count(tag) == 0) {
                const auto size = asset.find("size");
                out[tag] = {asset.value("browser_download_url", std::string()),
                            size != asset.end() && size->is_number_unsigned() ? size->get<std::uint64_t>() : 0};
            }
        }
    }
    return out;
}

/// The bytes of every file under `dir`.
std::uint64_t size_of(const std::filesystem::path& dir) {
    std::uint64_t   total = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code size_ec;
        if (it->is_regular_file(size_ec)) {
            const std::uint64_t size = it->file_size(size_ec);
            total += size_ec ? 0 : size;
        }
    }
    return total;
}

void tell(const std::function<void(std::string)>& say, std::string what) {
    if (say) {
        say(std::move(what));
    }
}

}  // namespace

std::string running_tag() { return CRUCIBLE_LLAMA_TAG; }

std::string chosen_tag() {
    const std::string tag = first_line(paths::runtimes_dir() / "active");
    if (!is_tag(tag)) {
        return {};
    }
    // One chosen under another Crucible, with no runtime for this one: the
    // launcher passed it over for the bundled one, and so does this.
    std::error_code ec;
    if (tag != bundled_tag() && !std::filesystem::exists(core_dir(tag) / "runtime.json", ec)) {
        return {};
    }
    return tag;
}

bool is_tag(std::string_view tag) {
    return tag.size() > 1 && tag[0] == 'b'
        && std::all_of(tag.begin() + 1, tag.end(), [](char c) { return c >= '0' && c <= '9'; });
}

long long tag_number(std::string_view tag) {
    if (!is_tag(tag) || tag.size() > 18) {
        return -1;
    }
    return std::stoll(std::string(tag.substr(1)));
}

std::filesystem::path core_dir(std::string_view tag) {
    return paths::runtimes_dir() / "core" / CRUCIBLE_VERSION / std::string(tag);
}

std::string core_asset(std::string_view tag) {
    return "runtime-core-" + prebuilt::platform_tag() + "-" + CRUCIBLE_VERSION + "-" + std::string(tag) + ".tar.gz";
}

std::vector<Version> list(std::string_view releases_json) {
    std::map<std::string, Version, std::less<>> found;
    const auto at = [&found](const std::string& tag) -> Version& {
        Version& version = found[tag];
        version.tag      = tag;
        return version;
    };
    const std::string bundled = bundled_tag();
    at(bundled).bundled   = true;
    at(bundled).installed = true;
    at(running_tag()).running   = true;
    at(running_tag()).installed = true;

    std::error_code ec;
    const std::filesystem::path cores = paths::runtimes_dir() / "core" / CRUCIBLE_VERSION;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(cores, ec)) {
        const std::string tag = entry.path().filename().string();
        if (is_tag(tag) && std::filesystem::exists(entry.path() / "runtime.json", ec)) {
            at(tag).installed = true;
        }
    }
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(paths::runtimes_dir() / "backends", ec)) {
        if (const std::string tag = entry.path().filename().string(); is_tag(tag)) {
            at(tag);
        }
    }
    for (const auto& [tag, where] : published(releases_json)) {
        Version& version = at(tag);
        version.url      = where.first;
        version.bytes    = where.second;
    }
    for (auto& [tag, version] : found) {
        version.disk = size_of(core_dir(tag)) + size_of(paths::runtimes_dir() / "backends" / tag);
    }
    // Chosen is what the next start loads: the bundled one when nothing is.
    const std::string chosen = chosen_tag();
    at(chosen.empty() ? bundled : chosen).chosen = true;

    std::vector<Version> out;
    out.reserve(found.size());
    for (auto& [tag, version] : found) {
        out.push_back(std::move(version));
    }
    std::sort(out.begin(), out.end(),
              [](const Version& a, const Version& b) { return tag_number(a.tag) > tag_number(b.tag); });
    return out;
}

bool install(std::string_view tag, std::string_view releases_json, std::string& error,
             const std::function<void(std::string)>& say) {
    if (!is_tag(tag)) {
        error = "\"" + std::string(tag) + "\" is not a llama.cpp version";
        return false;
    }
    const auto offered = published(releases_json);
    const auto found   = offered.find(tag);
    if (found == offered.end() || found->second.first.empty()) {
        error = "no runtime of Crucible " + std::string(CRUCIBLE_VERSION) + " is published with llama.cpp "
              + std::string(tag) + " for this system";
        return false;
    }
    const auto& [url, bytes] = found->second;

    // --- the program, built with it ---------------------------------------
    std::error_code             ec;
    const std::filesystem::path into    = core_dir(tag);
    const std::filesystem::path staging = into.parent_path() / ("." + std::string(tag) + "-unpacking");
    const std::filesystem::path archive = into.parent_path() / ("." + std::string(tag) + ".tar.gz");
    std::filesystem::create_directories(into.parent_path(), ec);
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);
    const std::string size = bytes > 0 ? format::bytes(bytes) : std::string();
    const std::string host = util::http::host_of(url);
    tell(say, "downloading " + core_asset(tag) + (size.empty() ? "" : " (" + size + ")")
                  + (host.empty() ? "" : " from " + host));
    const auto arrived = [&](std::uint64_t got) {
        tell(say, "downloading " + format::bytes(got) + (size.empty() ? "" : " of " + size));
    };
    if (std::string why; !util::http::download(url, archive, 1800, why, {}, arrived)) {
        error = "the download failed: " + why;
        std::filesystem::remove_all(staging, ec);
        std::filesystem::remove(archive, ec);
        return false;
    }
    tell(say, "unpacking");
    util::Subprocess tar;
    std::string      started;
    int              status = -1;
    if (tar.start({"tar", "-xzf", archive.string(), "-C", staging.string()}, {}, {}, started)) {
        std::string line;
        while (tar.read_line(line)) {
        }
        status = tar.wait();
    }
    std::filesystem::remove(archive, ec);
    // What it says it is, before it can be chosen: this Crucible, this llama.cpp.
    std::ifstream   in(staging / "runtime.json");
    const json      said = json::parse(in, nullptr, /*allow_exceptions=*/false);
    in.close();
    if (status != 0 || !said.is_object() || said.value("crucible", std::string()) != CRUCIBLE_VERSION
        || said.value("llama_tag", std::string()) != tag) {
        error = status != 0 ? "the runtime could not be unpacked" : "the download is not this Crucible's runtime";
        std::filesystem::remove_all(staging, ec);
        return false;
    }
    std::filesystem::remove_all(into, ec);
    std::filesystem::rename(staging, into, ec);
    if (ec) {
        error = "could not put it in place: " + ec.message();
        std::filesystem::remove_all(staging, ec);
        return false;
    }

    // --- its backends: each one this runtime has --------------------------
    const std::filesystem::path backends = paths::runtimes_dir() / "backends" / std::string(tag);
    std::filesystem::create_directories(backends, ec);
    json manifest = json::object();
    for (const RuntimeStatus& status_now : RuntimeRegistry::scan()) {
        if (!status_now.installed) {
            continue;
        }
        const BackendKind kind  = status_now.kind;
        const std::string asset = prebuilt::asset_name(kind, tag);
        std::uint64_t     asset_bytes = 0;
        const std::string asset_url   = prebuilt::find_asset(releases_json, asset, &asset_bytes);
        if (asset_url.empty()) {
            // Not fatal for a GPU's: the runtime offers it when it starts.
            // The processor's it cannot run a model without.
            if (backend_info(kind).required) {
                error = "llama.cpp " + std::string(tag) + " has no " + std::string(backend_info(kind).name)
                      + " runtime published for this system";
                return false;
            }
            tell(say, std::string(backend_info(kind).name) + " is not published for it: the runtime builds it");
            continue;
        }
        std::vector<std::filesystem::path> written;
        std::string                        why;
        if (!prebuilt::install(kind, asset_url, backends, written, why, say, asset_bytes)) {
            if (backend_info(kind).required) {
                error = std::string(backend_info(kind).name) + ": " + why;
                return false;
            }
            tell(say, std::string(backend_info(kind).name) + " was not installed: " + why);
            continue;
        }
        json entry = {{"llama_tag", std::string(tag)}, {"source", "downloaded"}};
        if (kind == BackendKind::Cuda) {
            entry["kernels"] = prebuilt::kCudaKernels;
        }
        manifest[std::string(backend_info(kind).id)] = std::move(entry);
        tell(say, std::string(backend_info(kind).name) + " ready");
    }
    if (std::ofstream out(backends / "manifest.json"); out) {
        out << manifest.dump(2) << '\n';
    }
    return true;
}

bool choose(std::string_view tag, std::string& error) {
    std::error_code             ec;
    const std::filesystem::path active = paths::runtimes_dir() / "active";
    if (tag.empty()) {
        std::filesystem::remove(active, ec);
        return true;
    }
    if (!is_tag(tag)) {
        error = "\"" + std::string(tag) + "\" is not a llama.cpp version";
        return false;
    }
    if (tag != bundled_tag() && !std::filesystem::exists(core_dir(tag) / "runtime.json", ec)) {
        error = "llama.cpp " + std::string(tag) + " is not installed";
        return false;
    }
    std::filesystem::create_directories(active.parent_path(), ec);
    std::ofstream out(active);
    out << tag << '\n';
    if (!out) {
        error = "could not write " + active.string();
        return false;
    }
    return true;
}

bool remove(std::string_view tag, std::string& error) {
    if (!is_tag(tag)) {
        error = "\"" + std::string(tag) + "\" is not a llama.cpp version";
        return false;
    }
    if (tag == running_tag()) {
        error = "llama.cpp " + std::string(tag) + " is running";
        return false;
    }
    if (tag == bundled_tag()) {
        error = "llama.cpp " + std::string(tag) + " came with Crucible and stays";
        return false;
    }
    const std::string chosen = chosen_tag();
    if (tag == (chosen.empty() ? bundled_tag() : chosen)) {
        error = "llama.cpp " + std::string(tag) + " is the one chosen for the next start";
        return false;
    }
    std::error_code ec;
    std::filesystem::remove_all(core_dir(tag), ec);
    std::filesystem::remove_all(paths::runtimes_dir() / "backends" / std::string(tag), ec);
    if (ec) {
        error = "could not delete it: " + ec.message();
        return false;
    }
    return true;
}

namespace {

/// The background's state. Never destroyed: a fetch's thread may outlive
/// everything else at exit, and must not find this gone.
struct Background {
    std::mutex   mutex;
    std::string  releases;
    std::int64_t checked = 0;
    std::string  check_error;
    Fetching     fetch;
};
Background& background() {
    static Background* state = new Background();
    return *state;
}

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

std::string releases() {
    const std::lock_guard<std::mutex> lock(background().mutex);
    return background().releases;
}

std::int64_t checked_at() {
    const std::lock_guard<std::mutex> lock(background().mutex);
    return background().checked;
}

std::string check_error() {
    const std::lock_guard<std::mutex> lock(background().mutex);
    return background().check_error;
}

bool refresh_releases(std::string& error) {
    std::string body;
    const bool  ok = prebuilt::fetch_releases(body, error);
    const std::lock_guard<std::mutex> lock(background().mutex);
    background().checked     = now_seconds();
    background().check_error = ok ? std::string() : error;
    if (ok) {
        background().releases = std::move(body);
    }
    return ok;
}

Fetching fetching() {
    const std::lock_guard<std::mutex> lock(background().mutex);
    return background().fetch;
}

bool start_fetch(std::string tag, bool automatic) {
    std::string list;
    {
        const std::lock_guard<std::mutex> lock(background().mutex);
        if (!background().fetch.tag.empty()) {
            return false;
        }
        Fetching& fetch = background().fetch;
        fetch.tag       = tag;
        fetch.said      = "starting";
        fetch.error     = {};
        list            = background().releases;
    }
    // Its own thread, detached: nothing it touches can go before it does.
    std::thread([tag = std::move(tag), list = std::move(list), automatic] {
        std::string error;
        bool        ok = install(tag, list, error, [](std::string step) {
            const std::lock_guard<std::mutex> lock(background().mutex);
            background().fetch.said = std::move(step);
        });
        if (ok && automatic) {
            ok = choose(tag, error);
        }
        const std::lock_guard<std::mutex> lock(background().mutex);
        Fetching& fetch = background().fetch;
        fetch.tag       = {};
        fetch.said      = {};
        fetch.error     = ok ? std::string() : error;
        if (ok) {
            fetch.ready     = tag;
            fetch.automatic = automatic;
        }
    }).detach();
    return true;
}

std::string newer_than_here() {
    const std::string chosen = chosen_tag();
    const long long   here   = std::max(tag_number(running_tag()), tag_number(chosen));
    std::string       best;
    for (const Version& version : list(releases())) {
        if (!version.url.empty() && tag_number(version.tag) > here
            && (best.empty() || tag_number(version.tag) > tag_number(best))) {
            best = version.tag;
        }
    }
    return best;
}

}  // namespace crucible::llama_versions
