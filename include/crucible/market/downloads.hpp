// SPDX-License-Identifier: MIT
//
// Models on their way into the models folder.
//
// A download from the Models tab is a few files of many gigabytes, and it has
// to survive what a long download meets: a slow line, a dropped connection, a
// window closed halfway. Each file is fetched into a part file beside where
// it goes and moved into place only once it is whole, and a part file left
// behind is carried on from, not started over -- by the next try, or by the
// same download asked for again after a restart.
//
// Each runs on a thread of its own and says how far it has got through
// `changed`, which the window turns into a push to the page: the page's
// progress bars are drawn from list(), not polled.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crucible/market/hub.hpp"

namespace crucible::market {

/// Fetch one file from `url` into `to`, by way of `to` plus ".part".
///
/// A part file already there is carried on from; one already as long as
/// `bytes` is only moved into place, and a file already at `to` with that
/// size is left alone. `bytes` is the size to expect, or 0 when the hub did
/// not say. `progress` is told this file's bytes so far, a few times a
/// second; `stop` is asked as often, and true ends the fetch with "stopped",
/// keeping the part file for the next try. A line that stalls ends it too --
/// stopped for going nowhere, rather than for taking long, which a download
/// of tens of gigabytes is entitled to do. `sha256`, when the hub keeps one,
/// is checked as the file comes in, and a file that arrives different from
/// it is removed rather than put in place.
bool fetch(const std::string& url, const std::filesystem::path& to, std::uint64_t bytes,
           const std::string& sha256, const std::function<void(std::uint64_t)>& progress,
           const std::function<bool()>& stop, std::string& error);

/// One download, as the page shows it.
struct Download {
    enum class State { Starting, Running, Done, Failed, Canceled };

    std::string           id;        ///< "unsloth/Qwen3-8B-GGUF:Q4_K_M"
    std::string           repo;      ///< the repository the option came from
    std::string           option;    ///< "Q4_K_M", "4bit"
    Format                format = Format::Gguf;
    std::filesystem::path folder;    ///< where it lands
    std::uint64_t         total = 0;
    std::uint64_t         done  = 0;
    double                rate  = 0;  ///< bytes a second, lately
    State                 state = State::Starting;
    std::string           error;
};

std::string_view state_id(Download::State state);   ///< "starting", "running", "done", "failed", "canceled"

class Downloads {
public:
    /// `changed` is called, from a download's own thread, when one starts,
    /// moves on or ends -- often enough for a bar, not for every byte.
    explicit Downloads(std::function<void()> changed);
    ~Downloads();
    Downloads(const Downloads&)            = delete;
    Downloads& operator=(const Downloads&) = delete;

    /// Begin fetching option `option` of `repo` into `models_dir`. What the
    /// option holds is asked of the hub again on the download's own thread,
    /// so this returns at once. False, with `error`, when the same one is
    /// already on its way.
    bool start(const std::string& repo, Format format, const std::string& option,
               const std::filesystem::path& models_dir, std::string& error);

    /// Stop one. What it had fetched is removed: stopping is not pausing.
    void cancel(const std::string& id);

    /// Forget one that is over, so the list shows only what matters.
    void dismiss(const std::string& id);

    std::vector<Download> list() const;

private:
    struct Job {
        Download          download;
        std::atomic<bool> canceled{false};
        std::thread       thread;
    };

    void run(const std::shared_ptr<Job>& job, std::filesystem::path models_dir);
    void update(const std::shared_ptr<Job>& job, const std::function<void(Download&)>& change,
                bool now);

    std::function<void()>             changed_;
    mutable std::mutex                mutex_;
    std::vector<std::shared_ptr<Job>> jobs_;
    std::atomic<bool>                 closing_{false};
};

}  // namespace crucible::market
