// SPDX-License-Identifier: MIT
//
// The Bench's datasets on this machine. Every page of rows fetched from
// Hugging Face's datasets server is kept, named for the rows it holds, and
// read back rather than asked for again: the server refuses an address that
// asks too often, and a run asks for a hundred pages and more. See
// engine_bench.cpp, and bench.py for how a run plans what to ask.
#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace crucible::bench_rows {

/// Where a dataset's pages are kept: <data>/bench/rows/<dataset__config__split>.
std::filesystem::path folder(const std::string& dataset, const std::string& config, const std::string& split);

/// The pages kept in `folder`, each as (offset, length), read from their
/// names: "4500-100.json" holds the 100 rows from row 4500. Sorted, and
/// anything else in the folder (a page half written, a stray file) left out.
std::vector<std::pair<int, int>> kept(const std::filesystem::path& folder);

}  // namespace crucible::bench_rows
