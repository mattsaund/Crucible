// SPDX-License-Identifier: MIT
#include "crucible/app/update.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "crucible/config/paths.hpp"
#include "crucible/util/http.hpp"
#include "crucible/util/platform.hpp"
#include "crucible/util/subprocess.hpp"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <unistd.h>
#endif

#ifndef CRUCIBLE_VERSION
#define CRUCIBLE_VERSION "0.0.0"
#endif

namespace crucible::update {
namespace {

using json = nlohmann::json;

/// The repository releases are published from. One copy, because a wrong URL
/// here is an update notice nobody ever sees rather than an error anybody sees.
constexpr const char* kRepository = "mattsaund/Crucible";

/// Split "1.2.3" into its numbers, ignoring a leading `v` and stopping at the
/// first thing that is not a digit or a dot. "0.5.0-rc1" gives {0, 5, 0}, and
/// the suffix is handled separately by compare().
std::vector<int> parts_of(std::string_view text) {
    std::vector<int> parts;
    std::size_t i = 0;
    if (i < text.size() && (text[i] == 'v' || text[i] == 'V')) {
        ++i;
    }
    int value = 0;
    bool digits = false;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            value = value * 10 + (c - '0');
            digits = true;
        } else if (c == '.') {
            parts.push_back(digits ? value : 0);
            value = 0;
            digits = false;
        } else {
            break;  // a pre-release suffix, or trailing prose
        }
    }
    if (digits) {
        parts.push_back(value);
    }
    return parts;
}

/// Whether a version carries a pre-release suffix: 0.5.0-rc1 is older than
/// 0.5.0, which is the one rule that makes a release candidate safe to publish.
bool pre_release(std::string_view text) {
    return text.find('-') != std::string_view::npos;
}

std::int64_t now_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

std::string_view current() {
    return CRUCIBLE_VERSION;
}

int compare(std::string_view a, std::string_view b) {
    const std::vector<int> left  = parts_of(a);
    const std::vector<int> right = parts_of(b);
    const std::size_t count = std::max(left.size(), right.size());
    for (std::size_t i = 0; i < count; ++i) {
        const int l = i < left.size() ? left[i] : 0;
        const int r = i < right.size() ? right[i] : 0;
        if (l != r) {
            return l < r ? -1 : 1;
        }
    }
    // Same numbers. A pre-release of a version is older than the version.
    const bool lp = pre_release(a);
    const bool rp = pre_release(b);
    if (lp == rp) {
        return 0;
    }
    return lp ? -1 : 1;
}

std::string releases_url() {
    return std::string("https://api.github.com/repos/") + kRepository
         + "/releases/latest";
}

bool parse_latest(std::string_view body, std::string& version, std::string& page) {
    const json parsed = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_object()) {
        return false;
    }
    const auto tag = parsed.find("tag_name");
    if (tag == parsed.end() || !tag->is_string()) {
        return false;  // a rate-limit note, an error, or a repo with no releases
    }
    version = tag->get<std::string>();
    if (version.empty()) {
        return false;
    }
    if (const auto url = parsed.find("html_url"); url != parsed.end() && url->is_string()) {
        page = url->get<std::string>();
    } else {
        page = std::string("https://github.com/") + kRepository + "/releases";
    }
    return true;
}

std::filesystem::path cache_file() {
    return paths::data_dir() / "update.json";
}

bool read_cache(State& state) {
    std::ifstream in(cache_file());
    if (!in) {
        return false;
    }
    const json parsed = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_object()) {
        return false;
    }
    state.latest     = parsed.value("latest", "");
    state.page       = parsed.value("page", "");
    state.checked_at = parsed.value("checked_at", std::int64_t{0});
    return true;
}

bool write_cache(const State& state) {
    std::error_code ec;
    std::filesystem::create_directories(cache_file().parent_path(), ec);
    std::ofstream out(cache_file());
    if (!out) {
        return false;
    }
    const json doc{
        {"latest", state.latest},
        {"page", state.page},
        {"checked_at", state.checked_at},
    };
    out << doc.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
    return out.good();
}

bool due(const State& state, std::int64_t now, std::int64_t every) {
    // A clock that has gone backwards -- a restored backup, a machine that was
    // wrong about the year until NTP corrected it -- would otherwise park the
    // next check somewhere in the future and never check again.
    if (state.checked_at > now) {
        return true;
    }
    return now - state.checked_at >= every;
}

bool newer_than_this(const State& state) {
    return !state.latest.empty() && compare(state.latest, current()) > 0;
}

std::string_view update_command() {
#if defined(_WIN32)
    return "irm https://raw.githubusercontent.com/mattsaund/Crucible/main/install.ps1 | iex";
#else
    return "curl -fsSL https://raw.githubusercontent.com/mattsaund/Crucible/main/install.sh | bash";
#endif
}

bool fetch(std::string& body, std::string& error) {
    util::http::Request request;
    request.url             = releases_url();
    request.headers         = {{"Accept", "application/vnd.github+json"}};
    request.timeout_seconds = 10;

    util::http::Response response = util::http::send(request);
    if (!response.ok()) {
        error = "the release list could not be reached (" + response.reason() + ")";
        return false;
    }
    body = std::move(response.body);
    return true;
}

State refresh(bool allowed_to_ask) {
    State state;
    read_cache(state);
    if (!allowed_to_ask || !due(state, now_seconds())) {
        return state;
    }

    std::string body;
    std::string error;
    if (!fetch(body, error)) {
        // A failed check is not news. Remember that it was tried, so a machine
        // with no network does not spawn a check on every start.
        state.checked_at = now_seconds();
        write_cache(state);
        return state;
    }

    std::string version;
    std::string page;
    if (parse_latest(body, version, page)) {
        state.latest = version;
        state.page   = page;
    }
    state.checked_at = now_seconds();
    write_cache(state);
    return state;
}

// ---------------------------------------------------------------------------
// Updating in place
// ---------------------------------------------------------------------------

namespace {

/// The .app this program runs from, or empty when it is not in one.
std::filesystem::path bundle_of(const std::filesystem::path& program) {
    for (std::filesystem::path at = program; at.has_parent_path() && at != at.parent_path();
         at = at.parent_path()) {
        if (at.extension() == ".app") {
            return at;
        }
    }
    return {};
}

/// The AppImage this program was started from, when it was.
std::filesystem::path appimage() {
    const char* path = std::getenv("APPIMAGE");
    return path != nullptr && *path != '\0' ? std::filesystem::path(path) : std::filesystem::path();
}

/// What the update replaces: the install folder (bin's parent) on Windows,
/// the .app on a Mac, the AppImage on Linux.
std::filesystem::path target_of(Install install) {
    const std::filesystem::path program = util::executable_path();
    switch (install) {
        case Install::Setup: {
            const std::filesystem::path bin = program.parent_path();
            return bin.filename() == "bin" ? bin.parent_path() : bin;
        }
        case Install::AppBundle: return bundle_of(program);
        case Install::AppImage:  return appimage();
        case Install::Source:    break;
    }
    return {};
}

/// A path for a POSIX shell: in single quotes, with any quote of its own
/// closed, escaped and reopened.
std::string sh(const std::filesystem::path& path) {
    std::string out = "'";
    for (const char c : path.string()) {
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return out + "'";
}

long this_process() {
#if defined(_WIN32)
    return static_cast<long>(::GetCurrentProcessId());
#else
    return static_cast<long>(::getpid());
#endif
}

}  // namespace

Install install_here() {
#if defined(_WIN32)
    // The setup program installs over whatever folder it is pointed at, so
    // an install.ps1 build updates the same way as a setup one.
    return Install::Setup;
#else
    if (!bundle_of(util::executable_path()).empty()) {
        return Install::AppBundle;
    }
    if (!appimage().empty()) {
        return Install::AppImage;
    }
    return Install::Source;
#endif
}

std::string asset_for(Install install) {
    switch (install) {
        case Install::Setup:     return "Crucible-Setup.exe";
        case Install::AppBundle: return "Crucible-macOS.dmg";
        case Install::AppImage:  return "Crucible-x86_64.AppImage";
        case Install::Source:    break;
    }
    return {};
}

bool find_asset(std::string_view body, std::string_view asset, std::string& url, std::uint64_t& bytes) {
    const json parsed = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_object() || !parsed.contains("assets") || !parsed["assets"].is_array()) {
        return false;
    }
    for (const json& one : parsed["assets"]) {
        if (one.is_object() && one.value("name", std::string()) == asset) {
            url   = one.value("browser_download_url", std::string());
            bytes = one.value("size", std::uint64_t{0});
            return !url.empty();
        }
    }
    return false;
}

std::string swap_script(Install install, long pid, const std::filesystem::path& fetched,
                        const std::filesystem::path& target) {
    const std::string id = std::to_string(pid);
    if (install == Install::Setup) {
        // cmd reads a script in the console's code page; 65001 is UTF-8, so a
        // folder with an accented name survives. The setup program shows its
        // own progress and, silent, starts nothing: the last line does.
        const std::string folder = target.string();
        return "@echo off\r\n"
               "chcp 65001 >nul\r\n"
               "rem Crucible's update: wait for it to close, install over it, start it again.\r\n"
               ":wait\r\n"
               "tasklist /NH /FI \"PID eq " + id + "\" 2>nul | find \"" + id + "\" >nul\r\n"
               "if not errorlevel 1 (\r\n"
               "  ping -n 2 127.0.0.1 >nul\r\n"
               "  goto wait\r\n"
               ")\r\n"
               "\"" + fetched.string() + "\" /SILENT /SUPPRESSMSGBOXES /NORESTART /DIR=\"" + folder + "\"\r\n"
               "start \"\" \"" + (target / "bin" / "crucible.exe").string() + "\"\r\n";
    }
    const std::string wait = "while kill -0 " + id + " 2>/dev/null; do sleep 0.5; done\n";
    if (install == Install::AppBundle) {
        // Copied beside the old one and swapped in only once the copy is
        // whole: a failure anywhere leaves the old Crucible where it was,
        // and it is opened either way.
        const std::filesystem::path fresh = target.string() + ".new";
        return "#!/bin/sh\n"
               "# Crucible's update: wait for it to close, put the new one in its place, open it.\n"
               + wait +
               "mount=$(mktemp -d) || exit 1\n"
               "if hdiutil attach -nobrowse -quiet -mountpoint \"$mount\" " + sh(fetched) + "; then\n"
               "  app=$(ls -d \"$mount\"/*.app | head -n 1)\n"
               "  rm -rf " + sh(fresh) + "\n"
               "  ditto \"$app\" " + sh(fresh) + " && rm -rf " + sh(target) + " && mv " + sh(fresh) + " "
               + sh(target) + "\n"
               "  hdiutil detach -quiet \"$mount\"\n"
               "fi\n"
               "rm -f " + sh(fetched) + "\n"
               "open " + sh(target) + "\n";
    }
    return "#!/bin/sh\n"
           "# Crucible's update: wait for it to close, put the new one in its place, start it.\n"
           + wait +
           "chmod +x " + sh(fetched) + " && mv -f " + sh(fetched) + " " + sh(target) + "\n"
           "exec " + sh(target) + "\n";
}

Installer::~Installer() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool Installer::start(std::function<void()> launched, std::string& error) {
    const Install install = install_here();
    if (asset_for(install).empty() || target_of(install).empty()) {
        error = "this Crucible was built from source -- update it with: " + std::string(update_command());
        return false;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    if (progress_.running()) {
        error = "an update is already under way";
        return false;
    }
    if (worker_.joinable()) {
        worker_.join();   // the last one, which has ended
    }
    progress_ = Progress{};
    progress_.phase = Phase::Asking;
    worker_ = std::thread([this, launched = std::move(launched)] { run(launched); });
    return true;
}

Installer::Progress Installer::progress() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    Progress out = progress_;
    if (out.phase == Phase::Downloading) {
        std::error_code ec;
        const std::uintmax_t got = std::filesystem::file_size(downloading_, ec);
        out.done = ec ? 0 : static_cast<std::uint64_t>(got);
    }
    return out;
}

void Installer::set(Phase phase, std::string error) {
    const std::lock_guard<std::mutex> lock(mutex_);
    progress_.phase = phase;
    progress_.error = std::move(error);
}

void Installer::run(std::function<void()> launched) {
    const Install install = install_here();
    const std::string asset = asset_for(install);

    std::string body;
    std::string error;
    if (!fetch(body, error)) {
        set(Phase::Failed, error);
        return;
    }
    std::string   url;
    std::uint64_t bytes = 0;
    if (!find_asset(body, asset, url, bytes)) {
        set(Phase::Failed, "the newest release has no " + asset);
        return;
    }

    const std::filesystem::path folder = paths::data_dir() / "update";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path part    = folder / (asset + ".part");
    const std::filesystem::path fetched = folder / asset;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        progress_.phase = Phase::Downloading;
        progress_.total = bytes;
        downloading_    = part;
    }
    if (!util::http::download(url, part, 1800, error)) {
        std::filesystem::remove(part, ec);
        set(Phase::Failed, "the download failed: " + error);
        return;
    }
    std::filesystem::remove(fetched, ec);
    std::filesystem::rename(part, fetched, ec);
    if (ec) {
        set(Phase::Failed, "the download could not be kept: " + ec.message());
        return;
    }

    set(Phase::Starting);
    const std::filesystem::path script = folder / (install == Install::Setup ? "update.cmd" : "update.sh");
    {
        std::ofstream out(script, std::ios::binary);
        out << swap_script(install, this_process(), fetched, target_of(install));
        if (!out) {
            set(Phase::Failed, "the update script could not be written");
            return;
        }
    }
    const std::vector<std::string> argv =
        install == Install::Setup ? std::vector<std::string>{"cmd", "/c", "\"" + script.string() + "\""}
                                  : std::vector<std::string>{"/bin/sh", script.string()};
    if (!util::start_detached(argv, error)) {
        set(Phase::Failed, error);
        return;
    }
    if (launched) {
        launched();
    }
}

}  // namespace crucible::update
