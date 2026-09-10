// SPDX-License-Identifier: MIT
//
// What the machine is doing, for the corner of the screen.
//
// Running a model is the most demanding thing most people ask of their
// hardware, and the two questions while it happens are always the same: is
// there room, and is it getting hot. Both are cheap to answer and neither is
// visible from inside llama.cpp.
//
// Reading is split from parsing so the parsing can be tested. The readings
// themselves come from `/proc` and `/sys`, which are free, and from
// `nvidia-smi`, which is not -- so it is sampled on a thread of its own rather
// than in the frame loop.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace crucible::util {

/// One device's state. Anything unreadable stays at -1, which the display shows
/// as nothing rather than as zero.
struct ResourceSample {
    std::string   name;
    std::uint64_t used  = 0;
    std::uint64_t total = 0;
    int           busy_percent  = -1;  ///< utilization
    int           temperature_c = -1;

    /// Memory used, 0-100, or -1 when the total is unknown.
    int memory_percent() const;
};

struct ResourceSnapshot {
    std::vector<ResourceSample> gpus;
    ResourceSample              cpu;
    /// False until the first sample has been taken.
    bool ready = false;
};

/// One `nvidia-smi --query-gpu=name,memory.used,memory.total,temperature.gpu,
/// utilization.gpu --format=csv,noheader,nounits` line. Megabytes in, bytes out.
/// Returns false for a line that is not one.
bool parse_gpu_line(std::string_view line, ResourceSample& into);

/// The `MemTotal:` and `MemAvailable:` of /proc/meminfo, in bytes.
/// MemAvailable rather than MemFree: free memory excludes the page cache, which
/// on a machine that has just read a 30 GB model is nearly all of it, and
/// reporting 98% used would be true of nothing anybody cares about.
bool parse_meminfo(std::string_view text, std::uint64_t& used, std::uint64_t& total);

/// The aggregate `cpu` line of /proc/stat, as (busy, total) jiffies. A
/// percentage needs two of these and the difference between them.
bool parse_stat(std::string_view text, std::uint64_t& busy, std::uint64_t& total);

/// The `model name` of /proc/cpuinfo, tidied: "12th Gen Intel(R) Core(TM)
/// i5-12400" is not a label, it is a legal notice.
std::string parse_cpu_name(std::string_view cpuinfo);

/// macOS `vm_stat` output, as (used, total) bytes.
///
/// There is no /proc on a Mac and no single figure for "available" either.
/// vm_stat counts pages, and what is genuinely in use is the pages that are
/// neither free nor speculative nor purgeable file cache -- the same
/// distinction MemAvailable draws on Linux, made by hand. `page_size` and
/// `total` come from sysctl, which this cannot ask for itself.
bool parse_vm_stat(std::string_view text, std::uint64_t page_size, std::uint64_t total,
                   std::uint64_t& used);

/// Samples the machine on a thread of its own and hands back the last reading.
///
/// `on_change` is called after each sample so the screen can redraw; it runs on
/// the sampling thread, so it must do no more than post an event.
class ResourceMonitor {
public:
    ResourceMonitor() = default;
    ~ResourceMonitor();
    ResourceMonitor(const ResourceMonitor&)            = delete;
    ResourceMonitor& operator=(const ResourceMonitor&) = delete;

    void start(std::function<void()> on_change);
    void stop();

    /// The most recent reading. Safe from any thread.
    ResourceSnapshot snapshot() const;

private:
    void run();

    mutable std::mutex mutex_;
    ResourceSnapshot   latest_;
    std::thread        worker_;
    std::atomic<bool>  running_{false};
    std::function<void()> on_change_;

    // The previous /proc/stat reading, for the difference that is a percentage.
    std::uint64_t last_busy_  = 0;
    std::uint64_t last_total_ = 0;
};

}  // namespace crucible::util
