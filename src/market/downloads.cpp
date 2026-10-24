// SPDX-License-Identifier: MIT
//
// See downloads.hpp.
#include "crucible/market/downloads.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>

#include "crucible/util/format.hpp"
#include "crucible/util/http.hpp"
#include "crucible/util/sha256.hpp"
#include "crucible/util/subprocess.hpp"

namespace crucible::market {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t size_of(const std::filesystem::path& file) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

std::filesystem::path part_of(const std::filesystem::path& file) { return file.string() + ".part"; }

/// The nearest folder that exists, at or above `path`: where free space can
/// be asked about before anything has been made.
std::filesystem::path existing_parent(std::filesystem::path path) {
    std::error_code ec;
    while (!path.empty() && !std::filesystem::exists(path, ec)) {
        const std::filesystem::path up = path.parent_path();
        if (up == path) {
            break;
        }
        path = up;
    }
    return path;
}

}  // namespace

std::string_view state_id(Download::State state) {
    switch (state) {
        case Download::State::Starting: return "starting";
        case Download::State::Running:  return "running";
        case Download::State::Done:     return "done";
        case Download::State::Failed:   return "failed";
        case Download::State::Canceled: return "canceled";
    }
    return "failed";
}

bool fetch(const std::string& url, const std::filesystem::path& to, std::uint64_t bytes,
           const std::string& sha256, const std::function<void(std::uint64_t)>& progress,
           const std::function<bool()>& stop, std::string& error) {
    if (!util::http::available(error)) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(to.parent_path(), ec);
    if (ec) {
        error = "could not make " + to.parent_path().string() + ": " + ec.message();
        return false;
    }
    if (bytes > 0 && std::filesystem::is_regular_file(to, ec) && size_of(to) == bytes) {
        progress(bytes);
        return true;   // already here, whole
    }

    const std::filesystem::path part = part_of(to);
    std::uint64_t have = size_of(part);
    // Longer than the file is a part file of something else; and without a
    // size to expect, there is no telling whether one is finished, so it is
    // started over rather than asked to carry on past its end.
    if ((bytes > 0 && have > bytes) || (bytes == 0 && have > 0)) {
        std::filesystem::remove(part, ec);
        have = 0;
    }

    // The digest is taken as the file grows, from what was just written and
    // is still in memory, so the check is all but done when the last byte
    // lands. The part file is only ever appended to: what has been read
    // stays read.
    util::Sha256      hash;
    std::uint64_t     hashed = 0;
    std::vector<char> chunk;
    const auto hash_to = [&](std::uint64_t end, std::uint64_t most) {
        if (sha256.empty() || end <= hashed) {
            return;
        }
        std::ifstream in(part, std::ios::binary);
        in.seekg(static_cast<std::streamoff>(hashed));
        chunk.resize(1 << 20);
        while (in && hashed < end && most > 0) {
            const std::uint64_t want = std::min<std::uint64_t>({chunk.size(), end - hashed, most});
            in.read(chunk.data(), static_cast<std::streamsize>(want));
            const std::streamsize got = in.gcount();
            if (got <= 0) {
                break;
            }
            hash.update(chunk.data(), static_cast<std::size_t>(got));
            hashed += static_cast<std::uint64_t>(got);
            most -= static_cast<std::uint64_t>(got);
        }
    };

    if (bytes == 0 || have < bytes) {
        util::Subprocess child;
        if (!child.start({"curl", "--location", "--fail", "--silent", "--show-error",
                          "--retry", "3", "--retry-delay", "3",
                          // Given up on when it stops moving -- under a kilobyte a
                          // second for a minute -- not after a set time.
                          "--speed-limit", "1024", "--speed-time", "60",
                          "--continue-at", "-", "--output", part.string(), url},
                         {}, {}, error)) {
            return false;
        }
        std::atomic<bool> finished{false};
        std::string       said;   // curl's last line, which is why it stopped
        std::thread reader([&child, &finished, &said] {
            std::string line;
            while (child.read_line(line)) {
                if (!line.empty()) {
                    said = line;
                }
            }
            finished.store(true);
        });
        bool stopped = false;
        while (!finished.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const std::uint64_t now = size_of(part);
            progress(now);
            hash_to(now, 128ULL << 20);   // a bounded share, so a stop is still heard
            // Only signaled from here, and the process only reaped below,
            // on this same thread, once the reader has seen it go: the two
            // cannot meet.
            if (!stopped && stop && stop()) {
                stopped = true;
                child.interrupt();
            }
        }
        reader.join();
        const int status = child.wait();
        if (stopped) {
            error = "stopped";
            return false;
        }
        if (status != 0) {
            error = said.empty() ? "the download failed (curl exited " + std::to_string(status) + ")" : said;
            return false;
        }
    }

    const std::uint64_t got = size_of(part);
    if (bytes > 0 && got != bytes) {
        error = to.filename().string() + " came to " + format::bytes(got) + " of "
              + format::bytes(bytes) + "; it was cut short";
        return false;
    }
    hash_to(got, got);
    if (!sha256.empty() && hashed == got && hash.hex() != sha256) {
        std::filesystem::remove(part, ec);
        error = to.filename().string() + " did not match its checksum and was removed";
        return false;
    }
    std::filesystem::rename(part, to, ec);
    if (ec) {
        error = "could not put " + to.filename().string() + " in place: " + ec.message();
        return false;
    }
    progress(got);
    return true;
}

Downloads::Downloads(std::function<void()> changed) : changed_(std::move(changed)) {}

Downloads::~Downloads() {
    // The window is closing: every download stops where it is, and its part
    // files stay for asking again to carry on from.
    closing_.store(true);
    std::vector<std::shared_ptr<Job>> jobs;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        jobs = jobs_;
    }
    for (const std::shared_ptr<Job>& job : jobs) {
        if (job->thread.joinable()) {
            job->thread.join();
        }
    }
}

bool Downloads::start(const std::string& repo, Format format, const std::string& option,
                      const std::filesystem::path& models_dir, std::string& error) {
    const std::string id = repo + ":" + option;
    std::shared_ptr<Job> job;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            const Download& d = (*it)->download;
            const bool over = d.state == Download::State::Done || d.state == Download::State::Failed
                           || d.state == Download::State::Canceled;
            if (d.id == id && !over) {
                error = "already downloading";
                return false;
            }
            // Asked for again after it ended: the old row goes.
            if (d.id == id && over) {
                if ((*it)->thread.joinable()) {
                    (*it)->thread.join();
                }
                it = jobs_.erase(it);
                continue;
            }
            ++it;
        }
        job                  = std::make_shared<Job>();
        job->download.id     = id;
        job->download.repo   = repo;
        job->download.option = option;
        job->download.format = format;
        job->download.folder = destination(models_dir, repo);
        jobs_.push_back(job);
    }
    job->thread = std::thread([this, job, models_dir] {
        // Nothing thrown here may leave the thread: it would end the program.
        try {
            run(job, models_dir);
        } catch (const std::exception& thrown) {
            update(job, [&thrown](Download& d) {
                d.state = Download::State::Failed;
                d.error = thrown.what();
            }, true);
        }
    });
    if (changed_) {
        changed_();
    }
    return true;
}

void Downloads::cancel(const std::string& id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const std::shared_ptr<Job>& job : jobs_) {
        if (job->download.id == id) {
            job->canceled.store(true);
        }
    }
}

void Downloads::dismiss(const std::string& id) {
    std::shared_ptr<Job> gone;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
            const Download::State s = (*it)->download.state;
            if ((*it)->download.id == id && s != Download::State::Starting && s != Download::State::Running) {
                gone = *it;
                jobs_.erase(it);
                break;
            }
        }
    }
    if (gone && gone->thread.joinable()) {
        gone->thread.join();   // over already: this returns at once
    }
    if (gone && changed_) {
        changed_();
    }
}

std::vector<Download> Downloads::list() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Download> out;
    out.reserve(jobs_.size());
    for (const std::shared_ptr<Job>& job : jobs_) {
        out.push_back(job->download);
    }
    return out;
}

void Downloads::update(const std::shared_ptr<Job>& job, const std::function<void(Download&)>& change,
                       bool now) {
    static thread_local Clock::time_point last;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        change(job->download);
    }
    // A bar moves a few times a second; every quarter-second tick of every
    // download would be a redraw of the whole page.
    if (changed_ && (now || Clock::now() - last > std::chrono::milliseconds(400))) {
        last = Clock::now();
        changed_();
    }
}

void Downloads::run(const std::shared_ptr<Job>& job, std::filesystem::path models_dir) {
    const auto fail = [this, &job](const std::string& why) {
        update(job, [&why](Download& d) { d.state = Download::State::Failed; d.error = why; }, true);
    };

    // What the option holds, asked again here rather than taken from the
    // page: the files and their sizes are the hub's to say.
    Detail      detail;
    std::string error;
    if (!model(job->download.repo, job->download.format, detail, error)) {
        return fail(error);
    }
    const auto found = std::find_if(detail.options.begin(), detail.options.end(),
                                    [&job](const Option& o) { return o.name == job->download.option; });
    if (found == detail.options.end()) {
        return fail(job->download.option + " is not offered any more");
    }
    const Option option = *found;
    const std::filesystem::path folder = destination(models_dir, option.repo);

    // What is here already counts, and what is left has to fit.
    std::uint64_t present = 0;
    for (const File& file : option.files) {
        const std::filesystem::path to = landing(folder, file, job->download.format);
        present += size_of(to) == file.bytes && file.bytes > 0 ? file.bytes : size_of(part_of(to));
    }
    const std::uint64_t left = option.bytes > present ? option.bytes - present : 0;
    std::error_code ec;
    const std::filesystem::space_info space = std::filesystem::space(existing_parent(folder), ec);
    if (!ec && space.available < left + (512ULL << 20)) {
        return fail("not enough space: " + format::bytes(left) + " needed, "
                    + format::bytes(space.available) + " free");
    }

    update(job, [&](Download& d) {
        d.folder = folder;
        d.total  = option.bytes;
        d.done   = present;
        d.state  = Download::State::Running;
    }, true);

    std::uint64_t     finished = 0;   // the files already done, in bytes
    std::uint64_t     seen     = present;
    Clock::time_point when     = Clock::now();
    const auto stop = [this, &job] { return job->canceled.load() || closing_.load(); };
    for (const File& file : option.files) {
        const std::filesystem::path to = landing(folder, file, job->download.format);
        const auto progress = [&](std::uint64_t now_bytes) {
            const std::uint64_t done = finished + now_bytes;
            const Clock::time_point t = Clock::now();
            const double seconds = std::chrono::duration<double>(t - when).count();
            update(job, [&](Download& d) {
                if (seconds > 0.2) {
                    const double instant = done > seen ? static_cast<double>(done - seen) / seconds : 0.0;
                    d.rate = d.rate <= 0 ? instant : d.rate * 0.7 + instant * 0.3;
                }
                d.done = std::min(done, d.total > 0 ? d.total : done);
            }, false);
            if (seconds > 0.2) {
                seen = done;
                when = t;
            }
        };
        if (!fetch(file_url(option.repo, file.path), to, file.bytes, file.sha256, progress, stop, error)) {
            if (job->canceled.load()) {
                // Canceled is not paused: what it fetched goes.
                for (const File& each : option.files) {
                    const std::filesystem::path path = landing(folder, each, job->download.format);
                    std::filesystem::remove(part_of(path), ec);
                }
                update(job, [](Download& d) { d.state = Download::State::Canceled; d.rate = 0; }, true);
                return;
            }
            if (closing_.load()) {
                return;   // the window is closing; the part files wait for next time
            }
            return fail(error);
        }
        finished += file.bytes > 0 ? file.bytes : size_of(to);
    }
    update(job, [](Download& d) {
        d.state = Download::State::Done;
        d.done  = d.total;
        d.rate  = 0;
    }, true);
}

}  // namespace crucible::market
