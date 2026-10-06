// SPDX-License-Identifier: MIT
//
// See link.hpp.
#include "crucible/orchestra/link.hpp"

#include <chrono>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "crucible/config/paths.hpp"
#include "crucible/lab/python.hpp"

namespace crucible::orchestra {

using json = nlohmann::json;

std::filesystem::path Link::log_file() { return paths::data_dir() / "orchestrator.log"; }

std::filesystem::path Link::package_dir() { return paths::data_dir() / "orchestrator"; }

Link::~Link() { stop(); }

bool Link::running() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return child_ && !ended_;
}

bool Link::start(std::string& error) {
    if (running()) {
        return true;
    }
    stop();   // whatever is left of one that went away

    const std::filesystem::path python = lab::python::interpreter();
    std::error_code ec;
    if (!lab::python::installed()) {
        error = "Crucible's Python is not installed yet";
        return false;
    }
    if (!write_package(package_dir(), error)) {
        return false;
    }
    std::filesystem::remove(log_file(), ec);

    // -E and -s: nothing from the environment or the user's site-packages,
    // so the orchestrator runs the same here as on the machine it was
    // tested on. -B: no bytecode written beside the package. -u: every line
    // leaves when it is written, not when a buffer fills.
    child_ = std::make_unique<util::Subprocess>();
    util::Subprocess::Streams streams;
    streams.input  = true;
    streams.errors = log_file();
    if (!child_->start({python.string(), "-E", "-s", "-B", "-u", "-m", "crucible_orchestrator"},
                       package_dir(), {}, error, streams)) {
        child_.reset();
        error = "the orchestrator could not be started: " + error;
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        ended_ = false;
    }

    util::Subprocess* child = child_.get();
    reader_ = std::thread([this, child] {
        std::string line;
        while (child->read_line(line)) {
            json message;
            try {
                message = json::parse(line);
            } catch (const json::exception&) {
                continue;   // not a message; the log has whatever it was
            }
            if (!message.is_object()) {
                continue;
            }
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                queue_.push_back(std::move(message));
            }
            arrived_.notify_all();
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            ended_ = true;
        }
        arrived_.notify_all();
    });

    // It says hello before anything else, with the protocol it speaks.
    json hello;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        arrived_.wait_for(lock, std::chrono::seconds(30),
                          [this] { return !queue_.empty() || ended_; });
        if (!queue_.empty()) {
            hello = std::move(queue_.front());
            queue_.pop_front();
        }
    }
    if (hello.value("method", "") != "hello") {
        error = gone("start");
        stop();
        return false;
    }
    const int protocol = hello.contains("params") ? hello["params"].value("protocol", 0) : 0;
    if (protocol != kProtocol) {
        error = "the orchestrator speaks protocol " + std::to_string(protocol) + ", and this "
                "Crucible speaks " + std::to_string(kProtocol);
        stop();
        return false;
    }
    return true;
}

void Link::stop() {
    if (!child_) {
        if (reader_.joinable()) {
            reader_.join();
        }
        return;
    }
    // Goodbye is the end of its input. A process that does not take the hint
    // within a few seconds is stopped.
    child_->close_input();
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!arrived_.wait_for(lock, std::chrono::seconds(3), [this] { return ended_; })) {
            lock.unlock();
            child_->interrupt();
        }
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    child_->wait();
    child_.reset();
    const std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    ended_ = true;
}

bool Link::send(const json& message) {
    return child_ && child_->write_line(message.dump(-1, ' ', false, json::error_handler_t::replace));
}

json Link::next() {
    std::unique_lock<std::mutex> lock(mutex_);
    arrived_.wait(lock, [this] { return !queue_.empty() || ended_; });
    if (queue_.empty()) {
        return nullptr;
    }
    json message = std::move(queue_.front());
    queue_.pop_front();
    return message;
}

std::string Link::gone(const std::string& what) const {
    // The last few lines it wrote to its log, which is where a Python
    // traceback says what happened.
    std::string said;
    std::vector<std::string> tail;
    if (std::ifstream in(log_file()); in) {
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) {
                tail.push_back(line);
            }
            if (tail.size() > 6) {
                tail.erase(tail.begin());
            }
        }
    }
    for (const std::string& line : tail) {
        said += "\n" + line;
    }
    return "the orchestrator stopped during " + what + said;
}

json Link::call(const std::string& method, const json& params, const Handler& handler) {
    const std::lock_guard<std::mutex> serial(call_mutex_);
    std::string error;
    if (!start(error)) {
        throw std::runtime_error(error);
    }
    const long wanted = ++next_id_;
    if (!send(json{{"id", wanted}, {"method", method}, {"params", params}})) {
        throw std::runtime_error(gone(method));
    }
    for (;;) {
        json message = next();
        if (message.is_null()) {
            throw std::runtime_error(gone(method));
        }
        if (message.contains("method")) {
            // Asked something in the middle of answering: answered here, on
            // this thread, which is the one that owns what it is asking about.
            if (!message.contains("id")) {
                continue;   // a notification; none is acted on yet
            }
            json reply{{"id", message["id"]}};
            try {
                const json asked = message.contains("params") ? message["params"] : json::object();
                reply["result"] = handler ? handler(message.value("method", ""), asked) : json();
            } catch (const std::exception& e) {
                reply["error"] = e.what();
            } catch (...) {
                reply["error"] = "the core could not answer";
            }
            if (!send(reply)) {
                throw std::runtime_error(gone(method));
            }
            continue;
        }
        if (!message.contains("id") || !message["id"].is_number() || message["id"].get<long>() != wanted) {
            continue;   // an answer to nothing this side is waiting for
        }
        if (message.contains("error") && !message["error"].is_null()) {
            throw std::runtime_error(message["error"].is_string() ? message["error"].get<std::string>()
                                                                  : message["error"].dump(-1, ' ', false, json::error_handler_t::replace));
        }
        return message.contains("result") ? message["result"] : json();
    }
}

}  // namespace crucible::orchestra
