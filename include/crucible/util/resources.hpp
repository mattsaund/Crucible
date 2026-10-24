// SPDX-License-Identifier: MIT
//
// How much memory the machine has, and how much of it is in use.
//
// Asked when it matters which models fit -- before one is loaded beside
// another, when a model to download is suggested -- and for the share of it
// the right-hand panel draws. None of it is visible from inside llama.cpp.
// The reading comes from /proc on Linux, from sysctl and vm_stat on a Mac,
// and from the Windows API.
//
// Reading is split from parsing so the parsing can be tested. The parsers for
// nvidia-smi, /proc/stat and /proc/cpuinfo are left from the monitor that
// sampled all of this for the corner of the screen, and only the tests call
// them now.
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

/// One device's state. Anything unreadable stays at -1 rather than reading as
/// zero.
struct ResourceSample {
    std::string   name;
    std::uint64_t used  = 0;
    std::uint64_t total = 0;
    int           busy_percent  = -1;  ///< utilization
    int           temperature_c = -1;

    /// Memory used, 0-100, or -1 when the total is unknown.
    int memory_percent() const;
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

/// The machine's memory as it is now, as (used, total) bytes. False when it
/// cannot be read. On a Mac this runs vm_stat, so it is for a decision -- may
/// another model be loaded beside this one -- and not for every frame.
bool system_memory(std::uint64_t& used, std::uint64_t& total);

}  // namespace crucible::util
