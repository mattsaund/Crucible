// SPDX-License-Identifier: MIT
//
// Programs left running. See processes.hpp.
#include "crucible/tools/processes.hpp"

#include "crucible/util/platform.hpp"
#include "crucible/util/text.hpp"

namespace crucible::tools {

Processes::~Processes() {
    stop_all();
}

std::string Processes::start(const std::string& command, const std::filesystem::path& cwd,
                             std::string& error) {
    if (command.empty()) {
        error = "START needs a command";
        return {};
    }
    auto entry     = std::make_shared<Entry>();
    entry->command = command;
    entry->child   = std::make_unique<util::Subprocess>();
    entry->started = std::chrono::steady_clock::now();
    if (!entry->child->start(util::shell_command(command), cwd, {}, error)) {
        return {};
    }

    // One reader per process, so a program that prints forever cannot fill
    // a pipe and stall. The tail is kept; the head is the part nobody reads.
    // Started before the entry is listed, so nothing can stop an entry whose
    // reader is not there yet.
    Entry* raw = entry.get();
    entry->reader = std::thread([raw] {
        std::string line;
        while (raw->child->read_line(line)) {
            const std::lock_guard<std::mutex> lock(raw->mutex);
            raw->output += detail::console_to_utf8(line);
            raw->output += '\n';
            if (raw->output.size() > 2 * kKeepBytes) {
                raw->output.erase(0, raw->output.size() - kKeepBytes);
            }
        }
        const int status = raw->child->wait();
        const std::lock_guard<std::mutex> lock(raw->mutex);
        raw->running = false;
        raw->status  = status;
    });

    const std::lock_guard<std::mutex> lock(mutex_);
    const std::string name = "p" + std::to_string(++next_);
    entries_[name] = std::move(entry);
    return name;
}

bool Processes::logs(const std::string& name, std::size_t max_bytes, std::string& out, bool& running,
                     int& status) {
    std::shared_ptr<Entry> entry;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(name);
        if (found == entries_.end()) {
            return false;
        }
        entry = found->second;
    }
    const std::lock_guard<std::mutex> lock(entry->mutex);
    out     = entry->output.size() > max_bytes ? entry->output.substr(entry->output.size() - max_bytes)
                                               : entry->output;
    running = entry->running;
    status  = entry->status;
    return true;
}

bool Processes::stop(const std::string& name, std::string& error) {
    std::shared_ptr<Entry> entry;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(name);
        if (found == entries_.end()) {
            error = "there is no process called " + name;
            return false;
        }
        entry = std::move(found->second);
        entries_.erase(found);
    }
    // Asked to stop, and killed if it will not: the reader's wait() sees to
    // that, so this join is a few seconds at the most.
    entry->child->terminate();
    if (entry->reader.joinable()) {
        entry->reader.join();
    }
    return true;
}

void Processes::stop_all() {
    std::map<std::string, std::shared_ptr<Entry>> taken;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        taken.swap(entries_);
    }
    // Every one asked first, so their grace runs side by side.
    for (auto& [name, entry] : taken) {
        entry->child->terminate();
    }
    for (auto& [name, entry] : taken) {
        if (entry->reader.joinable()) {
            entry->reader.join();
        }
    }
}

std::vector<ProcessInfo> Processes::list() {
    std::vector<ProcessInfo> out;
    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [name, entry] : entries_) {
        const std::lock_guard<std::mutex> each(entry->mutex);
        ProcessInfo info;
        info.name    = name;
        info.command = entry->command;
        info.running = entry->running;
        info.status  = entry->status;
        info.seconds = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now() - entry->started).count());
        out.push_back(std::move(info));
    }
    return out;
}

}  // namespace crucible::tools
