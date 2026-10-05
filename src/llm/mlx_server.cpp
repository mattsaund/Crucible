// SPDX-License-Identifier: MIT
//
// See mlx_server.hpp.
#include "crucible/llm/mlx_server.hpp"

#include <chrono>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#include "crucible/config/paths.hpp"
#include "crucible/lab/pyenv.hpp"
#include "crucible/util/http.hpp"
#include "crucible/util/subprocess.hpp"

#if !defined(_WIN32)
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace crucible::mlx {
namespace {

using Clock = std::chrono::steady_clock;

/// How long a model may take to come up. A large one is gigabytes read from
/// disk into memory, and a slow disk makes that minutes.
constexpr auto kStartLimit = std::chrono::minutes(15);

/// A port nothing is listening on: the system's choice, asked for by binding
/// to port 0 and reading back what it gave.
int free_port() {
#if defined(_WIN32)
    return 0;
#else
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port        = 0;
    int port = 0;
    socklen_t length = sizeof(address);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0
        && ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
        port = ntohs(address.sin_port);
    }
    ::close(fd);
    return port;
#endif
}

}  // namespace

bool is_model_dir(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)
        || !std::filesystem::is_regular_file(path / "config.json", ec)) {
        return false;
    }
    for (std::filesystem::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".safetensors") {
            return true;
        }
    }
    return false;
}

std::uintmax_t model_bytes(const std::filesystem::path& dir) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".safetensors") {
            std::error_code size_ec;
            const std::uintmax_t size = it->file_size(size_ec);
            total += size_ec ? 0 : size;
        }
    }
    return total;
}

std::string unavailable() {
#if defined(_WIN32)
    return "MLX does not run on Windows";
#else
    static std::mutex                 mutex;
    static std::string                answer;
    static Clock::time_point          asked{};
    static bool                       known = false;
    const std::lock_guard<std::mutex> lock(mutex);
    if (known && Clock::now() - asked < std::chrono::minutes(1)) {
        return answer;
    }
    asked = Clock::now();
    known = true;

    std::error_code ec;
    const std::filesystem::path python = lab::pyenv::python();
    if (!std::filesystem::exists(python, ec)) {
        answer = "MLX runs in Crucible's Python environment, which is not installed "
                 "-- install it in Settings, Training";
        return answer;
    }
    util::Subprocess probe;
    std::string error;
    if (!probe.start({python.string(), "-c", "import mlx_lm"}, {}, {}, error)) {
        answer = "Crucible's Python could not be started: " + error;
        return answer;
    }
    std::string line;
    while (probe.read_line(line)) {
    }
    answer = probe.wait() == 0
                 ? std::string()
                 : "Crucible's Python environment has no MLX in it -- it is installed with "
                   "the training environment on a Mac with Apple Silicon";
    return answer;
#endif
}

Server::Server() = default;

Server::~Server() { stop(); }

bool Server::serving(const std::filesystem::path& dir) const {
    return child_ && child_->running() && dir_ == dir;
}

std::string Server::base_url() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/v1";
}

std::string Server::tail() const {
    const std::lock_guard<std::mutex> lock(tail_mutex_);
    std::string out;
    for (const std::string& line : tail_) {
        out += (out.empty() ? "" : "\n") + line;
    }
    return out;
}

void Server::stop() {
    if (child_) {
        child_->terminate();
    }
    if (drain_.joinable()) {
        drain_.join();
    }
    if (child_) {
        child_->wait();
        child_.reset();
    }
    dir_.clear();
    port_ = 0;
}

bool Server::serve(const std::filesystem::path& dir, const std::function<bool()>& cancel,
                   std::string& error) {
    if (serving(dir)) {
        return true;
    }
    stop();
    if (const std::string why = unavailable(); !why.empty()) {
        error = why;
        return false;
    }
    port_ = free_port();
    if (port_ == 0) {
        error = "no port could be found for the MLX server";
        return false;
    }

    // "default_model" in every request is the model it was started with; the
    // path is the one place the model is named.
    const std::vector<std::string> argv{
        lab::pyenv::python().string(), "-m", "mlx_lm.server",
        "--model", dir.string(),
        "--host", "127.0.0.1",
        "--port", std::to_string(port_),
    };
    const std::vector<std::string> env{
        "PYTHONUNBUFFERED=1",
        // A local folder, never a download: a name that looked like a
        // Huggingface repository would otherwise be fetched.
        "HF_HUB_OFFLINE=1",
        "HF_HOME=" + (lab::pyenv::root() / "huggingface").string(),
    };
    child_ = std::make_unique<util::Subprocess>();
    if (!child_->start(argv, {}, env, error)) {
        child_.reset();
        error = "MLX could not be started: " + error;
        return false;
    }
    dir_ = dir;
    {
        const std::lock_guard<std::mutex> lock(tail_mutex_);
        tail_.clear();
    }

    // Read what it prints, all of it, for as long as it runs: a pipe nobody
    // reads fills up and stops the server mid-sentence. Kept in the data
    // folder beside llama.cpp's log, and its last lines in memory for an
    // error message.
    util::Subprocess* child = child_.get();
    drain_ = std::thread([this, child] {
        std::ofstream log(paths::data_dir() / "mlx-server.log", std::ios::trunc);
        std::string line;
        while (child->read_line(line)) {
            log << line << '\n';
            log.flush();
            const std::lock_guard<std::mutex> lock(tail_mutex_);
            tail_.push_back(line);
            if (tail_.size() > 12) {
                tail_.pop_front();
            }
        }
    });

    // Up when it answers. It reads the model before it listens, so the first
    // answer is also the model being ready.
    const auto started = Clock::now();
    while (Clock::now() - started < kStartLimit) {
        if (cancel && cancel()) {
            stop();
            error = "stopped";
            return false;
        }
        if (!child_->running()) {
            const std::string said = tail();
            stop();
            error = "MLX could not start " + dir.filename().string()
                  + (said.empty() ? std::string() : ":\n" + said);
            return false;
        }
        util::http::Request ask;
        ask.url             = base_url() + "/models";
        ask.timeout_seconds = 2;
        ask.connect_seconds = 1;
        if (util::http::send(ask).ok()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    stop();
    error = "MLX took more than fifteen minutes to start " + dir.filename().string();
    return false;
}

}  // namespace crucible::mlx
