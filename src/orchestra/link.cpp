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

void Link::set_handler(Handler handler) { handler_ = std::move(handler); }

bool Link::running() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return child_ && !ended_;
}

bool Link::start(std::string& error) {
    const std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (running()) {
        return true;
    }

    // Whatever is left of one that went away: its reader has ended, and the
    // threads that were answering it are joined before another starts.
    if (reader_.joinable()) {
        reader_.join();
    }
    reap();
    if (child_) {
        child_->wait();
        child_.reset();
    }

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
    auto child = std::make_unique<util::Subprocess>();
    util::Subprocess::Streams streams;
    streams.input  = true;
    streams.errors = log_file();
    if (!child->start({python.string(), "-E", "-s", "-B", "-u", "-m", "crucible_orchestrator"},
                      package_dir(), {}, error, streams)) {
        error = "the orchestrator could not be started: " + error;
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        child_ = std::move(child);
        hello_.reset();
        ended_ = false;
    }
    reader_ = std::thread([this, raw = child_.get()] { read_all(raw); });

    // It says hello before anything else, with the protocol it speaks.
    json hello;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        arrived_.wait_for(lock, std::chrono::seconds(30), [this] { return hello_.has_value() || ended_; });
        if (hello_) {
            hello = *hello_;
        }
    }
    const auto refuse = [this](std::string why, std::string& said) {
        said = std::move(why);
        child_->close_input();
        child_->interrupt();
        if (reader_.joinable()) {
            reader_.join();
        }
        child_->wait();
        const std::lock_guard<std::mutex> lock(mutex_);
        child_.reset();
        ended_ = true;
        return false;
    };
    if (hello.value("method", "") != "hello") {
        return refuse(gone("start"), error);
    }
    const int protocol = hello.contains("params") ? hello["params"].value("protocol", 0) : 0;
    if (protocol != kProtocol) {
        return refuse("the orchestrator speaks protocol " + std::to_string(protocol) + ", and this "
                      "Crucible speaks " + std::to_string(kProtocol), error);
    }
    return true;
}

void Link::read_all(util::Subprocess* child) {
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
        if (message.contains("method")) {
            if (message.value("method", "") == "hello") {
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    hello_ = std::move(message);
                }
                arrived_.notify_all();
                continue;
            }
            if (!message.contains("id")) {
                continue;   // a notification; none is acted on yet
            }
            // A request, answered on a thread of its own: it may wait a long
            // time -- for a model another seat holds, for the person to
            // answer a question -- and the next request must not wait for it.
            reap();
            auto task = std::make_unique<Answering>();
            Answering* raw = task.get();
            raw->thread = std::thread([this, raw, request = std::move(message)]() mutable {
                answer(std::move(request));
                raw->finished.store(true, std::memory_order_release);
            });
            const std::lock_guard<std::mutex> lock(mutex_);
            answering_.push_back(std::move(task));
            continue;
        }
        if (!message.contains("id") || !message["id"].is_number()) {
            continue;
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            const auto found = waiting_.find(message["id"].get<long>());
            if (found == waiting_.end()) {
                continue;   // an answer to nothing this side is waiting for
            }
            found->second->message = std::move(message);
            found->second->done    = true;
        }
        arrived_.notify_all();
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        ended_ = true;
    }
    arrived_.notify_all();
}

void Link::answer(json request) {
    json reply{{"id", request["id"]}};
    try {
        const json asked = request.contains("params") ? request["params"] : json::object();
        reply["result"] = handler_ ? handler_(request.value("method", ""), asked) : json();
    } catch (const std::exception& e) {
        reply["error"] = e.what();
    } catch (...) {
        reply["error"] = "the core could not answer";
    }
    send(reply);   // a pipe that has gone is the caller's to find out about
}

void Link::reap() {
    std::list<std::unique_ptr<Answering>> finished;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = answering_.begin(); it != answering_.end();) {
            if ((*it)->finished.load(std::memory_order_acquire)) {
                finished.push_back(std::move(*it));
                it = answering_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const std::unique_ptr<Answering>& task : finished) {
        task->thread.join();
    }
}

void Link::stop() {
    const std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (child_) {
        // Goodbye is the end of its input. A process that does not take the
        // hint within a few seconds is stopped.
        child_->close_input();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!arrived_.wait_for(lock, std::chrono::seconds(3), [this] { return ended_; })) {
                lock.unlock();
                child_->interrupt();
            }
        }
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    // Every request still being answered: its caller has gone, so its
    // answer goes nowhere, but the thread must finish before the engine it
    // reaches into does.
    std::list<std::unique_ptr<Answering>> all;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        all.swap(answering_);
    }
    for (const std::unique_ptr<Answering>& task : all) {
        task->thread.join();
    }
    if (child_) {
        child_->wait();
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    child_.reset();
    ended_ = true;
}

bool Link::send(const json& message) {
    const std::lock_guard<std::mutex> one_line(send_mutex_);
    util::Subprocess* child = nullptr;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (ended_ || !child_) {
            return false;
        }
        child = child_.get();
    }
    return child->write_line(message.dump(-1, ' ', false, json::error_handler_t::replace));
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

json Link::call(const std::string& method, const json& params) {
    std::string error;
    if (!start(error)) {
        throw std::runtime_error(error);
    }
    Waiter waiter;
    long   wanted = 0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        wanted = ++next_id_;
        waiting_[wanted] = &waiter;
    }
    const auto forget = [this, wanted] {
        const std::lock_guard<std::mutex> lock(mutex_);
        waiting_.erase(wanted);
    };
    if (!send(json{{"id", wanted}, {"method", method}, {"params", params}})) {
        forget();
        throw std::runtime_error(gone(method));
    }
    {
        std::unique_lock<std::mutex> lock(mutex_);
        arrived_.wait(lock, [this, &waiter] { return waiter.done || ended_; });
        waiting_.erase(wanted);
        if (!waiter.done) {
            lock.unlock();
            throw std::runtime_error(gone(method));
        }
    }
    const json& message = waiter.message;
    if (message.contains("error") && !message["error"].is_null()) {
        throw std::runtime_error(message["error"].is_string() ? message["error"].get<std::string>()
                                                              : message["error"].dump(-1, ' ', false, json::error_handler_t::replace));
    }
    return message.contains("result") ? message["result"] : json();
}

}  // namespace crucible::orchestra
