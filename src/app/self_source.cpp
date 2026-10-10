// SPDX-License-Identifier: MIT
//
// See self_source.hpp.
#include "crucible/app/self_source.hpp"

#include <fstream>
#include <iterator>
#include <system_error>
#include <thread>

#include "crucible/util/platform.hpp"
#include "crucible/util/subprocess.hpp"

namespace crucible::self {
namespace {

namespace fs = std::filesystem;

#if defined(_WIN32)
constexpr const char* kProgram = "crucible.exe";
#else
constexpr const char* kProgram = "crucible";
#endif

/// The program a build folder makes: bin/ under it, or under its
/// configuration on a generator that keeps one per configuration.
fs::path program_in(const fs::path& build) {
    std::error_code ec;
    for (const fs::path& candidate : {build / "bin" / kProgram, build / "bin" / "Release" / kProgram,
                                      build / "bin" / "RelWithDebInfo" / kProgram}) {
        if (fs::exists(candidate, ec)) {
            return candidate;
        }
    }
    return build / "bin" / kProgram;
}

}  // namespace

bool is_source(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir / "src" / "engine" / "engine.cpp", ec)) {
        return false;
    }
    std::ifstream in(dir / "CMakeLists.txt");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text.find("project(Crucible") != std::string::npos || text.find("project(crucible") != std::string::npos;
}

Source find(const fs::path& chosen) {
    Source source;
    std::error_code ec;
    if (!chosen.empty()) {
        if (!is_source(chosen)) {
            source.why = chosen.string() + " is not Crucible's source";
            return source;
        }
        source.found   = true;
        source.root    = chosen;
        source.build   = chosen / "build";
        source.program = program_in(source.build);
        return source;
    }
    // Walking up from the running program: a build folder is somewhere above
    // it -- the one with CMakeCache.txt -- and the source somewhere above that.
    const fs::path exe = util::executable_path();
    fs::path build;
    for (fs::path at = exe.parent_path(); !at.empty() && at != at.parent_path(); at = at.parent_path()) {
        if (build.empty() && fs::exists(at / "CMakeCache.txt", ec)) {
            build = at;
        }
        if (is_source(at)) {
            source.found   = true;
            source.root    = at;
            source.build   = build.empty() ? at / "build" : build;
            source.program = program_in(source.build);
            return source;
        }
    }
    source.why = "this copy was installed from a download and has no source beside it";
    return source;
}

Rebuild::~Rebuild() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Rebuild::add(const std::string& line) {
    const std::lock_guard<std::mutex> lock(mutex_);
    lines_.push_back(line);
    if (lines_.size() > 400) {
        lines_.erase(lines_.begin(), lines_.begin() + 100);
    }
}

std::string Rebuild::log() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::string out;
    const std::size_t from = lines_.size() > 120 ? lines_.size() - 120 : 0;
    for (std::size_t i = from; i < lines_.size(); ++i) {
        out += lines_[i];
        out += '\n';
    }
    return out;
}

bool Rebuild::start(const Source& source, std::string& error) {
    if (!source.found) {
        error = source.why.empty() ? std::string("there is no source to build") : source.why;
        return false;
    }
    if (running_.exchange(true)) {
        error = "a rebuild is already running";
        return false;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        lines_.clear();
    }
    status_.store(-1);
    worker_ = std::thread([this, source] {
        const auto step = [this](const std::vector<std::string>& argv, const fs::path& cwd) {
            std::string line = "$";
            for (const std::string& part : argv) {
                line += " " + part;
            }
            add(line);
            util::Subprocess child;
            std::string error;
            if (!child.start(argv, cwd, {}, error)) {
                add(error);
                return 127;
            }
            std::string out;
            while (child.read_line(out)) {
                add(out);
            }
            return child.wait();
        };
        int status = 0;
        std::error_code ec;
        if (!fs::exists(source.build / "CMakeCache.txt", ec)) {
            status = step({"cmake", "-S", source.root.string(), "-B", source.build.string(),
                           "-DCMAKE_BUILD_TYPE=Release"}, source.root);
        }
        if (status == 0) {
            const unsigned jobs = std::max(1U, std::thread::hardware_concurrency());
            status = step({"cmake", "--build", source.build.string(), "--target", "crucible", "crucible-app",
                           "-j", std::to_string(jobs)}, source.root);
        }
        add(status == 0 ? "built" : "the build failed");
        status_.store(status);
        running_.store(false);
    });
    return true;
}

bool restart(const Source& source, std::string& error) {
    std::error_code ec;
    if (!fs::exists(source.program, ec)) {
        error = source.program.string() + " is not there -- rebuild first";
        return false;
    }
    // Its own session, so closing this program does not take the new one.
#if defined(_WIN32)
    util::Subprocess child;
    if (!child.start({"cmd", "/c", "start", "", source.program.string()}, source.program.parent_path(), {}, error)) {
        return false;
    }
    child.wait();
#else
    // A subshell put in the background: the new program is the shell's
    // grandchild, handed to the system when the shell exits, and outlives
    // this one. (A Mac has no setsid to do it the other way.)
    util::Subprocess child;
    if (!child.start({"/bin/sh", "-c", "(\"$0\" >/dev/null 2>&1 </dev/null &)", source.program.string()},
                     source.program.parent_path(), {}, error)) {
        return false;
    }
    child.wait();
#endif
    return true;
}

}  // namespace crucible::self
