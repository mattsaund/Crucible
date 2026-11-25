// SPDX-License-Identifier: MIT
//
// See mlx_runtime.hpp.
#include "crucible/lab/mlx_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "crucible/lab/pyenv.hpp"
#include "crucible/util/subprocess.hpp"

namespace crucible::lab::mlx {
namespace {

using json = nlohmann::json;

/// What pyenv::plan asks for, so an update keeps to the same bounds.
const std::vector<std::string>& requirements() {
    static const std::vector<std::string> wanted = [] {
        for (const pyenv::Step& step : pyenv::plan(pyenv::Flavor::Mlx)) {
            if (step.label == "MLX") {
                return step.packages;
            }
        }
        return std::vector<std::string>{"mlx", "mlx-lm"};
    }();
    return wanted;
}

/// The state that outlives a call: the last check, an update under way.
/// Never destroyed, as a detached update's thread may outlive the rest.
struct Background {
    std::mutex   mutex;
    std::string  newest_mlx;
    std::string  newest_mlx_lm;
    std::int64_t checked_at = 0;
    std::string  check_error;
    bool         updating = false;
    std::string  said;
    std::string  error;
};
Background& background() {
    static Background* state = new Background();
    return *state;
}

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// pip's names are compared folded: "mlx-lm", "mlx_lm" and "MLX_LM" are one.
std::string folded(std::string_view name) {
    std::string out;
    for (const char c : name) {
        out += c == '-' || c == '.' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

/// A command run to the end, each line handed to `line`. Its exit status.
int run(const std::vector<std::string>& argv, std::string& error, const std::function<void(const std::string&)>& line) {
    util::Subprocess child;
    if (!child.start(argv, {}, {}, error)) {
        return -1;
    }
    std::string got;
    while (child.read_line(got)) {
        if (line) {
            line(got);
        }
    }
    return child.wait();
}

}  // namespace

std::filesystem::path site_packages() {
    const std::filesystem::path venv = pyenv::venv_dir();
#if defined(_WIN32)
    return venv / "Lib" / "site-packages";
#else
    // lib/python3.X/site-packages, whichever Python made it.
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(venv / "lib", ec)) {
        if (entry.path().filename().string().rfind("python", 0) == 0
            && std::filesystem::is_directory(entry.path() / "site-packages", ec)) {
            return entry.path() / "site-packages";
        }
    }
    return venv / "lib" / "site-packages";
#endif
}

std::string version_in(std::string_view folder, std::string_view package) {
    const std::string_view suffix = ".dist-info";
    if (folder.size() <= suffix.size() || folder.substr(folder.size() - suffix.size()) != suffix) {
        return {};
    }
    const std::string_view stem = folder.substr(0, folder.size() - suffix.size());
    // The name, a dash, the version: the version has no dash, the name may.
    const std::size_t dash = stem.rfind('-');
    if (dash == std::string_view::npos || folded(stem.substr(0, dash)) != folded(package)) {
        return {};
    }
    return std::string(stem.substr(dash + 1));
}

bool read_report(std::string_view report, std::string& mlx, std::string& mlx_lm) {
    // The object alone: a warning pip prints can come in the same stream.
    const std::size_t open  = report.find('{');
    const std::size_t close = report.rfind('}');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
        return false;
    }
    const json parsed = json::parse(report.substr(open, close - open + 1), nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_object() || !parsed.contains("install") || !parsed["install"].is_array()) {
        return false;
    }
    for (const json& item : parsed["install"]) {
        const json metadata = item.value("metadata", json::object());
        const std::string name    = folded(metadata.value("name", std::string()));
        const std::string version = metadata.value("version", std::string());
        if (name == "mlx") {
            mlx = version;
        } else if (name == "mlx_lm") {
            mlx_lm = version;
        }
    }
    return true;
}

Versions versions() {
    Versions out;
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(site_packages(), ec)) {
        const std::string name = entry.path().filename().string();
        if (std::string v = version_in(name, "mlx"); !v.empty()) {
            out.mlx = std::move(v);
        } else if (std::string lm = version_in(name, "mlx-lm"); !lm.empty()) {
            out.mlx_lm = std::move(lm);
        }
    }
    out.present = !out.mlx.empty();
    const std::lock_guard<std::mutex> lock(background().mutex);
    const Background& known = background();
    // What a check found that is already here reads as here.
    out.newest_mlx    = known.newest_mlx.empty() ? out.mlx : known.newest_mlx;
    out.newest_mlx_lm = known.newest_mlx_lm.empty() ? out.mlx_lm : known.newest_mlx_lm;
    out.checked_at    = known.checked_at;
    out.check_error   = known.check_error;
    out.updating      = known.updating;
    out.said          = known.said;
    out.error         = known.error;
    return out;
}

bool check(std::string& error) {
    if (!versions().present) {
        error = "MLX is not installed";
        return false;
    }
    // What pip would do, in JSON on its standard output, and nothing done.
    std::vector<std::string> argv = {pyenv::python().string(), "-m", "pip", "install", "--dry-run", "--upgrade",
                                     "--quiet", "--disable-pip-version-check", "--report", "-"};
    argv.insert(argv.end(), requirements().begin(), requirements().end());
    std::string report;
    const int   status = run(argv, error, [&report](const std::string& line) { report += line + "\n"; });
    // Nothing in the report's install list is nothing newer: what is here.
    std::string mlx;
    std::string mlx_lm;
    const bool  read = status == 0 && read_report(report, mlx, mlx_lm);
    const std::lock_guard<std::mutex> lock(background().mutex);
    background().checked_at = now_seconds();
    if (!read) {
        error = error.empty() ? "pip could not say what is newer" : error;
        background().check_error = error;
        return false;
    }
    background().check_error   = {};
    background().newest_mlx    = mlx;
    background().newest_mlx_lm = mlx_lm;
    return true;
}

bool update_available() {
    const Versions now = versions();
    return now.present && ((!now.newest_mlx.empty() && now.newest_mlx != now.mlx)
                           || (!now.newest_mlx_lm.empty() && now.newest_mlx_lm != now.mlx_lm));
}

bool start_update(std::string mlx_lm) {
    if (!versions().present) {
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(background().mutex);
        if (background().updating) {
            return false;
        }
        background().updating = true;
        background().said     = "starting";
        background().error    = {};
    }
    std::thread([mlx_lm = std::move(mlx_lm)] {
        std::vector<std::string> argv = {pyenv::python().string(), "-m", "pip", "install", "--upgrade",
                                         "--disable-pip-version-check", "--progress-bar", "off"};
        if (mlx_lm.empty()) {
            argv.insert(argv.end(), requirements().begin(), requirements().end());
        } else {
            argv.push_back("mlx-lm==" + mlx_lm);
        }
        std::string error;
        const int   status = run(argv, error, [](const std::string& line) {
            if (line.rfind("Collecting", 0) == 0 || line.rfind("Downloading", 0) == 0
                || line.rfind("Installing", 0) == 0 || line.rfind("Successfully", 0) == 0) {
                const std::lock_guard<std::mutex> lock(background().mutex);
                background().said = line;
            }
        });
        const std::lock_guard<std::mutex> lock(background().mutex);
        background().updating = false;
        background().said     = {};
        if (status != 0) {
            background().error = error.empty() ? "pip could not update MLX" : error;
            return;
        }
        // What is installed now is what a check would say is newest.
        background().newest_mlx    = {};
        background().newest_mlx_lm = {};
    }).detach();
    return true;
}

}  // namespace crucible::lab::mlx
