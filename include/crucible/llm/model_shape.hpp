// SPDX-License-Identifier: MIT
//
// What a GGUF says about itself, read without loading it.
//
// Two things need to know how much room a model will want before committing to
// it: the GPU split, which fills cards in order and therefore has to know when
// one is full, and the "Dedicated VRAM only" check, which refuses a model
// rather than letting the driver spill it into system RAM.
//
// Both used to guess -- a flat fraction of the file size -- and a guess is
// exactly wrong here. Too generous and it refuses models that fit; too mean and
// it waves through ones that do not. A 32 GB model was refused on 34.8 GB of
// free VRAM because a 25% allowance invented eight gigabytes of overhead that
// were never going to be used.
//
// The real numbers are in the file. GGUF puts its metadata at the front --
// including the name, shape and type of every tensor -- ggml can read it
// without touching tensor data, and it costs about 50 ms even for a 34 GB
// model. So nothing here is estimated that can be read, and the two figures
// that genuinely cannot be (the compute buffers) are named as allowances.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace crucible {

/// One piece of a model as llama.cpp places it: a whole block, or the output.
///
/// llama.cpp divides a model by layer, never by byte. `tensor_split` is turned
/// into cumulative fractions and block `i` goes to the first device whose
/// fraction exceeds `i / (n_block + 1)`, so a plan expressed in bytes only ever
/// lands on the nearest whole block -- a 680 MB step on a 48-block model, which
/// is enough to overfill a card that the arithmetic said had room. Planning in
/// these units instead removes the rounding altogether.
///
/// The list runs in llama.cpp's own placement order: block 0 first, the output
/// last. See ModelShape::units.
struct ModelUnit {
    std::uint64_t weights      = 0;  ///< the tensors in this unit
    std::uint64_t kv_per_token = 0;  ///< 0 for a block that keeps no KV cache
    std::uint64_t state        = 0;  ///< recurrent (SSM) state, fixed size
    bool          sliding      = false;  ///< attends to the last ModelShape::window tokens only
};

/// The parts of a model's memory footprint that are knowable before loading.
struct ModelShape {
    /// False when the file is missing, is not a GGUF, or does not carry the
    /// keys this needs. Callers fall back to their own conservative guess
    /// rather than refusing something they cannot measure.
    bool known = false;

    std::uint64_t weights = 0;  ///< the file, which is what the tensors weigh
    std::uint32_t layers  = 0;
    std::uint32_t vocab   = 0;

    /// The context the model was trained to, or zero when the file does not
    /// say: as far as a context grown to fit the cards may go.
    std::uint32_t trained = 0;

    /// The input embedding, which llama.cpp keeps in system memory whatever
    /// the offload settings say -- "there is very little benefit to offloading
    /// the input layer", llama-model.cpp. It is part of `weights` but never
    /// part of what a card is asked to hold, so it is named separately rather
    /// than counted against video memory.
    std::uint64_t host_weights = 0;

    /// Bytes of KV cache one token of context costs, summed over every layer.
    /// Zero for an architecture whose attention shape could not be read.
    std::uint64_t kv_per_token = 0;

    /// How far back a sliding-window layer attends, in tokens, or zero for a
    /// model without such layers. gpt-oss attends 128 back on every other
    /// layer; flint 513 back on most of its 49.
    std::uint32_t window = 0;

    /// The part of `kv_per_token` on the sliding-window layers: what a window
    /// cache stops charging for every token past the window.
    std::uint64_t sliding_kv_per_token = 0;

    /// Every placeable unit, in llama.cpp's order: `layers` blocks then the
    /// output. Empty when the tensor table could not be read, which is the
    /// signal to fall back to proportional splitting.
    std::vector<ModelUnit> units;

    /// How many cells a sliding-window layer keeps in a context of `n_ctx`.
    ///
    /// llama.cpp keeps the whole context on those layers too unless told not
    /// to (swa_full), because a cache holding only the window cannot be
    /// rewound past it. Told not to, it keeps the window and one batch beyond
    /// it, padded to 256: 1280 cells for flint, against 32768 at 32K -- a
    /// window for each of the context's `sequences`. Zero, which every figure
    /// below reads as the whole context, when `window_cache` is off or the
    /// model has no sliding window.
    std::uint64_t sliding_cells(int n_ctx, int n_batch, bool window_cache, int sequences = 1) const;

    /// KV cache at `n_ctx` tokens. `sliding` is what each sliding-window
    /// layer keeps instead of the whole context; see sliding_cells.
    std::uint64_t kv_bytes(int n_ctx, std::uint64_t sliding = 0) const;

    /// The recurrent state a hybrid model keeps: fixed-size, independent of
    /// the context length, and zero for an ordinary transformer.
    std::uint64_t state_bytes() const;

    /// What one unit costs on the card that holds it, cache included.
    std::uint64_t unit_bytes(std::size_t index, int n_ctx, std::uint64_t sliding = 0) const;

    /// The logits buffer. Large -- a 152k vocabulary at a batch of 512 is
    /// 300 MB -- and it lives only on the card holding the output unit, which
    /// is why it is not part of the per-card allowance.
    std::uint64_t logit_bytes(int n_batch) const;

    /// The activations and workspace one card needs to run its share of the
    /// graph, over and above the weights and the cache. This is the one figure
    /// here that is an allowance rather than a reading: it depends on the shape
    /// of the compute graph, which is not in the header.
    std::uint64_t compute_bytes(int n_batch) const;

    /// Everything: weights, cache and compute.
    std::uint64_t total_bytes(int n_ctx, int n_batch, std::uint64_t sliding = 0) const;

    /// Weights plus cache and state -- the part that is divided between cards,
    /// since all of it follows the layers. The compute buffers do not, and are
    /// covered by the per-card headroom in devices.hpp instead.
    std::uint64_t resident_bytes(int n_ctx, std::uint64_t sliding = 0) const;
};

/// Read `file`'s header. Cheap: metadata only, no tensor data.
ModelShape read_model_shape(const std::filesystem::path& file);

}  // namespace crucible
