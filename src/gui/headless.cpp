// SPDX-License-Identifier: MIT
//
// The session with no window: crucible-engine.
//
// What cruciblecli talks to. It is the same App the window draws, the same
// api::Surface in front of it, and this file is only what window.cpp is for
// the page: requests come in, replies and the state go out. Here they are one
// JSON document a line, on this process's stdin and stdout.
//
//   in:   {"id": 7, "method": "submit", "params": {...}}
//   out:  {"id": 7, "ok": true, "result": ...}
//   out:  {"push": <the state, as the page is pushed it>}
//
// When stdin ends the program it belongs to has gone, so this ends too: no
// engine is left holding a model on a card after a terminal is closed.
#include "app.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <fcntl.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "crucible/tools/terminal.hpp"
#include "crucible/api/surface.hpp"
#include "crucible/app/starter.hpp"
#include "crucible/util/resources.hpp"

namespace crucible::gui {
namespace {

/// What the session's thread waits on.
struct Bus {
    std::mutex              mutex;
    std::condition_variable changed;
    std::deque<std::string> requests;
    bool                    dirty   = true;    ///< the state has moved since it was last sent
    bool                    closing = false;
};

/// The one running, for App::quit.
std::atomic<Bus*> running{nullptr};

void close_bus(Bus& bus) {
    {
        const std::lock_guard<std::mutex> lock(bus.mutex);
        bus.closing = true;
    }
    bus.changed.notify_all();
}

/// Everything that goes out, on a thread of its own, so a terminal that is
/// slow to read never holds the session up. Replies are all sent, in order.
/// Of the state only the newest waiting is: an older one has nothing to add.
class Writer {
public:
    Writer(int fd, std::shared_ptr<Bus> bus) : fd_(fd), bus_(std::move(bus)) {
        thread_ = std::thread([this] { run(); });
    }

    ~Writer() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            closing_ = true;
        }
        changed_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    Writer(const Writer&)            = delete;
    Writer& operator=(const Writer&) = delete;

    void reply(std::string line) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            replies_.push_back(std::move(line));
        }
        changed_.notify_one();
    }

    void state(std::string line) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            state_ = std::move(line);
        }
        changed_.notify_one();
    }

private:
    bool write_all(const std::string& text) const {
        const char* at   = text.data();
        std::size_t left = text.size();
        while (left > 0) {
#if defined(_WIN32)
            const int wrote = _write(fd_, at, static_cast<unsigned int>(std::min<std::size_t>(left, 1U << 24)));
#else
            const auto wrote = ::write(fd_, at, left);
#endif
            if (wrote <= 0) {
                return false;
            }
            at += wrote;
            left -= static_cast<std::size_t>(wrote);
        }
        return true;
    }

    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            changed_.wait(lock, [this] { return closing_ || !replies_.empty() || !state_.empty(); });
            if (replies_.empty() && state_.empty()) {
                return;   // closing, with everything said
            }
            std::string next;
            if (!replies_.empty()) {
                next = std::move(replies_.front());
                replies_.pop_front();
            } else {
                next = std::move(state_);
                state_.clear();
            }
            lock.unlock();
            next += '\n';
            const bool sent = write_all(next);
            lock.lock();
            if (!sent) {
                // Nobody is reading. Whatever is left has nowhere to go.
                replies_.clear();
                state_.clear();
                close_bus(*bus_);
                if (closing_) {
                    return;
                }
            }
        }
    }

    int                     fd_;
    std::shared_ptr<Bus>    bus_;
    std::mutex              mutex_;
    std::condition_variable changed_;
    std::deque<std::string> replies_;
    std::string             state_;
    bool                    closing_ = false;
    std::thread             thread_;
};

/// The two ends of the conversation.
struct Pipes {
    int in  = -1;
    int out = -1;
};

/// This process's stdin and stdout, taken for the protocol and kept from every
/// program it starts. A command an expert runs would otherwise inherit them:
/// one that reads its input would eat requests, and one that prints would put
/// a broken document in the middle of the conversation. What is left in their
/// places is an empty input, and stderr for anything a library prints.
Pipes take_pipes() {
    std::fflush(stdout);
    Pipes pipes;
#if defined(_WIN32)
    pipes.in  = _dup(_fileno(stdin));
    pipes.out = _dup(_fileno(stdout));
    _setmode(pipes.in, _O_BINARY);
    _setmode(pipes.out, _O_BINARY);
    SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(pipes.in)), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(pipes.out)), HANDLE_FLAG_INHERIT, 0);
    const int nothing = _open("NUL", _O_RDONLY);
    if (nothing >= 0) {
        _dup2(nothing, _fileno(stdin));
        _close(nothing);
    }
    _dup2(_fileno(stderr), _fileno(stdout));
    SetStdHandle(STD_INPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdin))));
    SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
#else
    pipes.in  = ::fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
    pipes.out = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    const int nothing = ::open("/dev/null", O_RDONLY);
    if (nothing >= 0) {
        ::dup2(nothing, STDIN_FILENO);
        ::close(nothing);
    }
    ::dup2(STDERR_FILENO, STDOUT_FILENO);
    // cruciblecli gone is a write that fails, which ends the session in good
    // order. Left to the signal, it would end the process on the spot.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    return pipes;
}

/// One request at a time off the pipe, until the other end has gone.
void read_requests(int fd, Bus& bus) {
    std::string held;
    char        chunk[65536];
    for (;;) {
#if defined(_WIN32)
        const int got = _read(fd, chunk, static_cast<unsigned int>(sizeof(chunk)));
#else
        const auto got = ::read(fd, chunk, sizeof(chunk));
        if (got < 0 && errno == EINTR) {
            continue;
        }
#endif
        if (got <= 0) {
            break;
        }
        held.append(chunk, static_cast<std::size_t>(got));
        std::size_t from = 0;
        for (std::size_t end = held.find('\n'); end != std::string::npos; end = held.find('\n', from)) {
            std::string line = held.substr(from, end - from);
            from = end + 1;
            if (line.empty()) {
                continue;
            }
            {
                const std::lock_guard<std::mutex> lock(bus.mutex);
                bus.requests.push_back(std::move(line));
            }
            bus.changed.notify_one();
        }
        held.erase(0, from);
    }
    close_bus(bus);
}

/// As often as the state is sent: faster than text can be read as it streams.
constexpr std::chrono::milliseconds kInterval{33};

}  // namespace

int App::run() {
    const Pipes pipes = take_pipes();

    // Whatever the download did not carry, as the window fetches it.
    if (starter::needed(config_)) {
        std::uint64_t used  = 0;
        std::uint64_t total = 0;
        util::system_memory(used, total);
        setup_.want_model(starter::for_memory(total), config_.resolved_models_dir());
    }
    setup_.start();

    const auto surface = std::make_shared<api::Surface>(static_cast<api::Host&>(*this));
    const auto bus     = std::make_shared<Bus>();
    const auto writer  = std::make_shared<Writer>(pipes.out, bus);
    running.store(bus.get());
    set_wake([bus] {
        {
            const std::lock_guard<std::mutex> lock(bus->mutex);
            bus->dirty = true;
        }
        bus->changed.notify_one();
    });

    // Left running when the session ends: it is blocked in a read that only
    // the other end can finish, and the process is about to exit.
    std::thread([bus, in = pipes.in] { read_requests(in, *bus); }).detach();

    engine_->start();
    workers_.post([this] { engine_->prepare_orchestrator(); });
    begin_update_check();
    refresh_prices();

    using Clock = std::chrono::steady_clock;
    Clock::time_point next_state = Clock::now();
    for (;;) {
        std::deque<std::string> requests;
        bool send_state = false;
        bool closing    = false;
        {
            std::unique_lock<std::mutex> lock(bus->mutex);
            const auto woken = [&] { return bus->closing || !bus->requests.empty(); };
            if (bus->dirty) {
                bus->changed.wait_until(lock, next_state, woken);
            } else {
                bus->changed.wait(lock, [&] { return woken() || bus->dirty; });
            }
            requests.swap(bus->requests);
            closing = bus->closing;
            if (bus->dirty && Clock::now() >= next_state) {
                bus->dirty = false;
                send_state = true;
            }
        }

        for (const std::string& request : requests) {
            api::Prepared call = surface->prepare(request);
            if (call.background) {
                // A lookup: slow, and touching nothing of the session's.
                workers_.post([surface, writer, call = std::move(call)] { writer->reply(surface->run(call)); });
                continue;
            }
            writer->reply(surface->run(call));
            // Something only the session knows may have changed with it.
            const std::lock_guard<std::mutex> lock(bus->mutex);
            bus->dirty = true;
        }
        if (closing) {
            break;
        }
        if (!send_state) {
            continue;
        }

        // Contained, as in the window: nothing here may end the session.
        try {
            housekeeping();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "housekeeping failed: %s\n", e.what());
            say(std::string("something went wrong saving: ") + e.what());
        } catch (...) {
            std::fprintf(stderr, "housekeeping failed\n");
        }
        try {
            writer->state("{\"push\":" + surface->page_snapshot() + "}");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "snapshot failed: %s\n", e.what());
        }
        next_state = Clock::now() + kInterval;
    }

    running.store(nullptr);
    engine_->stop();
    // No shell outlives the program it was opened in.
    tools::terminal::close_all();
    set_wake(nullptr);
    return 0;
}

std::string App::restart() {
    // A program on the other end of a pipe is not started again from here:
    // its next start, whoever makes it, runs on what was chosen.
    return "the engine runs on it from its next start";
}

void App::quit() {
    if (Bus* const bus = running.load()) {
        close_bus(*bus);
    }
}

}  // namespace crucible::gui
