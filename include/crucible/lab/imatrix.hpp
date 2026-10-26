// SPDX-License-Identifier: MIT
//
// Importance matrices: which of a weight's inputs matter, measured by running
// the model over sample text.
//
// The lowest-bit quantizations (IQ1, IQ2, IQ3_XXS) keep so little of each
// weight that where the bits go decides whether the model still works: an
// importance matrix says, for every weight, how large the values multiplied
// into each of its columns ran while the model read ordinary text, and the
// quantizer spends its precision there. llama.cpp refuses those types without
// one, and makes one with its llama-imatrix tool; this is the same measurement
// done inside Crucible, so condensing to IQ2_XXS is one choice in the Create
// tab rather than a tool, a text file and a command line.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace crucible::lab {

/// For each weight, by name, the mean square of every input column it was
/// given; for a mixture-of-experts weight, of each expert's columns in turn.
/// What llama.cpp's quantizer takes as an importance matrix.
struct ImportanceMatrix {
    std::map<std::string, std::vector<float>> values;
    int chunks = 0;   ///< how many chunks of sample text it was measured over
};

/// What a weight's sums come to once divided by how many rows were seen:
/// `sums` holds `counts.size()` runs of `sums.size() / counts.size()` columns.
/// An expert that was never chosen counts as ones, which asks nothing of the
/// quantizer.
std::vector<float> mean_squares(const std::vector<float>& sums, const std::vector<std::int64_t>& counts);

/// Text to measure with: the start of WikiText-2's training split, fetched
/// from Hugging Face's datasets server once and kept. Not the benchmarks'
/// questions: a matrix measured on them would tune a condensed model to the
/// test it is then given.
bool sample_text(std::string& text, std::string& error);

/// Measure `model` over `text`, `chunk_tokens` at a time, for at most
/// `max_chunks` chunks. The model goes on the cards as far as their free
/// memory takes it and on the CPU for the rest. `progress` is told (chunks
/// done, chunks in all); `cancel` is asked between chunks.
bool measure_importance(const std::filesystem::path& model, const std::string& text, ImportanceMatrix& out,
                        std::string& error, const std::function<void(int, int)>& progress = {},
                        const std::function<bool()>& cancel = {}, int chunk_tokens = 512, int max_chunks = 100);

/// Where the matrix for `model` is kept: by its path, size and time, so
/// condensing one model to several sizes measures it once, and a model that
/// changed is measured again.
std::filesystem::path importance_file(const std::filesystem::path& model);

bool save_importance(const ImportanceMatrix& matrix, const std::filesystem::path& file, std::string& error);
bool load_importance(const std::filesystem::path& file, ImportanceMatrix& matrix);

}  // namespace crucible::lab
