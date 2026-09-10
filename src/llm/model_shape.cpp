// SPDX-License-Identifier: MIT
//
// Reading a GGUF header. See model_shape.hpp for why this is read and not
// estimated.
#include "crucible/llm/model_shape.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gguf.h>

namespace crucible {
namespace {

/// Two bytes per element: llama.cpp's default KV cache is f16, and a quantised
/// cache would only ever be smaller. Erring large is the safe direction for
/// every caller here.
constexpr std::uint64_t kKvElementBytes = 2;

/// Four bytes per element of recurrent state: llama.cpp allocates both the
/// convolution window and the SSM state as f32.
constexpr std::uint64_t kStateElementBytes = 4;

/// What one card needs beyond the weights and the cache, before the logits.
///
/// Two things, and the second is the larger. The compute buffer llama.cpp
/// reserves and reports is 130 MiB for a 48-block model with a 2048-wide
/// embedding at a batch of 512. What it does not report is what CUDA itself
/// takes once the graph runs -- cuBLAS workspaces, graph capture, the driver's
/// own bookkeeping -- which on the same model measured between 150 and 450 MB
/// per card, most on the card doing the most work.
///
/// So this is 512 MiB: enough for both, on the evidence. It is the only
/// allowance in this file; everything else is read from the header.
constexpr std::uint64_t kActivationAllowance = 512ULL * 1024 * 1024;

std::string string_value(gguf_context* gguf, const char* key) {
    const std::int64_t id = gguf_find_key(gguf, key);
    if (id < 0 || gguf_get_kv_type(gguf, id) != GGUF_TYPE_STRING) {
        return {};
    }
    return gguf_get_val_str(gguf, id);
}

/// One unsigned value, or the per-layer list of them.
///
/// Both spellings are in the wild for the same key: a plain model writes
/// `attention.head_count_kv` as a number, while a hybrid one -- LFM2, and
/// anything else that alternates attention with something cheaper -- writes an
/// array with a zero for every layer that has no KV cache at all.
///
/// gguf_get_val_* aborts the process on a type mismatch, so every read here is
/// behind a type check.
std::vector<std::uint64_t> unsigned_values(gguf_context* gguf, const std::string& key) {
    const std::int64_t id = gguf_find_key(gguf, key.c_str());
    if (id < 0) {
        return {};
    }

    const gguf_type type = gguf_get_kv_type(gguf, id);
    if (type == GGUF_TYPE_ARRAY) {
        const gguf_type element = gguf_get_arr_type(gguf, id);
        if (element != GGUF_TYPE_INT32 && element != GGUF_TYPE_UINT32) {
            return {};
        }
        const auto* data = static_cast<const std::int32_t*>(gguf_get_arr_data(gguf, id));
        if (data == nullptr) {
            return {};
        }
        std::vector<std::uint64_t> values;
        values.reserve(gguf_get_arr_n(gguf, id));
        for (std::size_t i = 0; i < gguf_get_arr_n(gguf, id); ++i) {
            values.push_back(data[i] < 0 ? 0 : static_cast<std::uint64_t>(data[i]));
        }
        return values;
    }
    if (type == GGUF_TYPE_UINT32) {
        return {gguf_get_val_u32(gguf, id)};
    }
    if (type == GGUF_TYPE_INT32) {
        const std::int32_t value = gguf_get_val_i32(gguf, id);
        return {value < 0 ? 0 : static_cast<std::uint64_t>(value)};
    }
    return {};
}

std::uint64_t first_or(const std::vector<std::uint64_t>& values, std::uint64_t fallback) {
    return values.empty() ? fallback : values.front();
}

/// The block a tensor belongs to, or -1 for one that belongs to none.
///
/// Every architecture names its repeating tensors `blk.<n>.<what>`, which is
/// the only naming convention this file relies on -- and the one llama.cpp
/// itself builds every model from, so a file that broke it would not load.
std::int64_t block_of(std::string_view name, std::string_view& suffix) {
    constexpr std::string_view kPrefix = "blk.";
    if (name.substr(0, kPrefix.size()) != kPrefix) {
        return -1;
    }
    const std::size_t dot = name.find('.', kPrefix.size());
    if (dot == std::string_view::npos) {
        return -1;
    }
    const std::string digits(name.substr(kPrefix.size(), dot - kPrefix.size()));
    if (digits.empty() ||
        !std::all_of(digits.begin(), digits.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return -1;
    }
    suffix = name.substr(dot + 1);
    return std::strtoll(digits.c_str(), nullptr, 10);
}

/// What one block of the model looks like, gathered from its tensors.
struct BlockTensors {
    std::uint64_t weights = 0;

    // The width of the K and V projections, which is exactly the number of
    // cached elements per token: n_head_kv * head_dim, without having to know
    // either factor. Zero when the block has no separate K/V projection, which
    // is the case for a linear-attention block and for a fused QKV.
    std::uint64_t k_width = 0;
    std::uint64_t v_width = 0;

    bool has_projection = false;  ///< a Q/K/V projection: this block attends
    bool has_recurrent  = false;  ///< an ssm_* tensor: a state rather than a cache

    std::uint64_t conv_channels = 0;  ///< ssm_conv1d.weight's row count
    std::uint64_t conv_width    = 0;  ///< ssm_conv1d.weight's kernel size
};

}  // namespace

std::uint64_t ModelShape::kv_bytes(int n_ctx) const {
    if (n_ctx <= 0 || kv_per_token == 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(n_ctx) * kv_per_token;
}

std::uint64_t ModelShape::state_bytes() const {
    std::uint64_t total = 0;
    for (const ModelUnit& unit : units) {
        total += unit.state;
    }
    return total;
}

std::uint64_t ModelShape::unit_bytes(std::size_t index, int n_ctx) const {
    if (index >= units.size()) {
        return 0;
    }
    const ModelUnit& unit = units[index];
    return unit.weights + unit.state +
           unit.kv_per_token * static_cast<std::uint64_t>(std::max(0, n_ctx));
}

std::uint64_t ModelShape::logit_bytes(int n_batch) const {
    const auto batch = static_cast<std::uint64_t>(std::max(1, n_batch));
    return static_cast<std::uint64_t>(vocab) * batch * sizeof(float);
}

std::uint64_t ModelShape::compute_bytes(int n_batch) const {
    return kActivationAllowance + logit_bytes(n_batch);
}

std::uint64_t ModelShape::resident_bytes(int n_ctx) const {
    // The input embedding is deliberately left in: this is what the model
    // costs, and the caller that cares which side of the bus it lands on
    // subtracts host_weights itself.
    return weights + kv_bytes(n_ctx) + state_bytes();
}

std::uint64_t ModelShape::total_bytes(int n_ctx, int n_batch) const {
    return resident_bytes(n_ctx) + compute_bytes(n_batch);
}

ModelShape read_model_shape(const std::filesystem::path& file) {
    ModelShape shape;

    std::error_code ec;
    shape.weights = std::filesystem::file_size(file, ec);
    if (ec) {
        shape.weights = 0;
        return shape;
    }

    gguf_init_params params{};
    params.no_alloc = true;   // metadata only: no tensor data is read
    params.ctx      = nullptr;
    gguf_context* gguf = gguf_init_from_file(file.string().c_str(), params);
    if (gguf == nullptr) {
        return shape;  // not a GGUF; the file size is still worth having
    }

    const std::string arch = string_value(gguf, "general.architecture");
    if (arch.empty()) {
        gguf_free(gguf);
        return shape;
    }

    const auto key = [&arch](const char* suffix) { return arch + "." + suffix; };

    const std::uint64_t layers = first_or(unsigned_values(gguf, key("block_count")), 0);
    const std::uint64_t embd   = first_or(unsigned_values(gguf, key("embedding_length")), 0);
    const std::uint64_t heads  = first_or(unsigned_values(gguf, key("attention.head_count")), 0);
    const std::vector<std::uint64_t> kv_heads =
        unsigned_values(gguf, key("attention.head_count_kv"));

    // Where a model states its head dimensions, use them. Deriving them as
    // embedding / heads is only right when the two happen to divide evenly,
    // which is not true of every architecture -- qwen3next states 256 against
    // an embedding of 2048 over 16 heads, which would derive as 128.
    const std::uint64_t key_len =
        first_or(unsigned_values(gguf, key("attention.key_length")),
                 heads > 0 ? embd / heads : 0);
    const std::uint64_t value_len =
        first_or(unsigned_values(gguf, key("attention.value_length")), key_len);
    const std::uint64_t ssm_inner = first_or(unsigned_values(gguf, key("ssm.inner_size")), 0);
    const std::uint64_t ssm_state = first_or(unsigned_values(gguf, key("ssm.state_size")), 0);

    shape.layers    = static_cast<std::uint32_t>(layers);
    shape.train_ctx = static_cast<std::uint32_t>(
        first_or(unsigned_values(gguf, key("context_length")), 0));

    // The vocabulary, for the logits buffer. Its size is the length of the
    // token list rather than a key of its own in most files.
    if (const std::int64_t tokens = gguf_find_key(gguf, "tokenizer.ggml.tokens");
        tokens >= 0 && gguf_get_kv_type(gguf, tokens) == GGUF_TYPE_ARRAY) {
        shape.vocab = static_cast<std::uint32_t>(gguf_get_arr_n(gguf, tokens));
    } else {
        shape.vocab = static_cast<std::uint32_t>(
            first_or(unsigned_values(gguf, key("vocab_size")), 0));
    }

    // --- walk the tensor table ---------------------------------------------
    //
    // Every tensor is named, shaped and typed in the header, so what each
    // block weighs and whether it caches anything are both readable facts
    // rather than things to work out from the architecture. That matters most
    // for the hybrid models this would otherwise get badly wrong: a qwen3next
    // caches on one block in four, and charging it for all 48 invents 600 MB
    // of cache that is never allocated.
    std::vector<BlockTensors> blocks(layers);
    std::uint64_t output_weights = 0;

    for (std::int64_t i = 0; i < gguf_get_n_tensors(gguf); ++i) {
        const char* raw = gguf_get_tensor_name(gguf, i);
        if (raw == nullptr) {
            continue;
        }
        const std::string_view name(raw);
        const std::uint64_t    size = gguf_get_tensor_size(gguf, i);
        const std::int64_t*    ne   = gguf_get_tensor_ne(gguf, i);

        std::string_view suffix;
        const std::int64_t block = block_of(name, suffix);
        if (block < 0 || block >= static_cast<std::int64_t>(layers)) {
            if (name == "token_embd.weight") {
                // Never offloaded, whatever n_gpu_layers says. See
                // ModelShape::host_weights.
                shape.host_weights += size;
            } else {
                output_weights += size;
            }
            continue;
        }

        BlockTensors& into = blocks[static_cast<std::size_t>(block)];
        into.weights += size;

        // Only a projection counts as attention. A normalization weight is
        // named attn_norm on blocks that never attend at all -- LFM2 puts one
        // on every short-convolution block -- and treating that as attention
        // would invent a cache for two blocks in three.
        if (suffix == "attn_k.weight" || suffix == "attn_v.weight" ||
            suffix == "attn_q.weight" || suffix == "attn_qkv.weight") {
            into.has_projection = true;
            // ne[1] of the K projection is n_head_kv * head_dim -- the cached
            // width per token, read without needing either factor.
            if (suffix == "attn_k.weight" && ne != nullptr) {
                into.k_width = static_cast<std::uint64_t>(ne[1]);
            } else if (suffix == "attn_v.weight" && ne != nullptr) {
                into.v_width = static_cast<std::uint64_t>(ne[1]);
            }
        }
        if (suffix.substr(0, 4) == "ssm_") {
            into.has_recurrent = true;
            if (suffix == "ssm_conv1d.weight" && ne != nullptr) {
                into.conv_width    = static_cast<std::uint64_t>(ne[0]);
                into.conv_channels = static_cast<std::uint64_t>(ne[1]);
            }
        }
    }

    // --- turn that into placeable units ------------------------------------
    //
    // The cache per layer stated by the header, for a block whose own tensors
    // did not say. Zero for a layer a hybrid model lists as having no cache.
    const auto stated_kv = [&](std::uint64_t index) {
        const std::uint64_t count =
            kv_heads.size() == 1
                ? kv_heads.front()
                : (index < kv_heads.size() ? kv_heads[static_cast<std::size_t>(index)] : 0);
        return count * (key_len + value_len) * kKvElementBytes;
    };

    const bool have_tensors =
        std::any_of(blocks.begin(), blocks.end(),
                    [](const BlockTensors& block) { return block.weights > 0; });

    if (have_tensors) {
        shape.units.reserve(layers + 1);
        for (std::uint64_t index = 0; index < layers; ++index) {
            const BlockTensors& block = blocks[static_cast<std::size_t>(index)];
            ModelUnit unit;
            unit.weights = block.weights;

            if (block.k_width > 0) {
                unit.kv_per_token = (block.k_width + block.v_width) * kKvElementBytes;
            } else if (block.has_projection && !block.has_recurrent) {
                // A fused QKV projection: the widths are not separable from the
                // tensor, so fall back to what the header states.
                unit.kv_per_token = stated_kv(index);
            }

            if (block.has_recurrent) {
                // The convolution window, plus the SSM state itself. Both are
                // f32 and both are per sequence, of which Crucible runs one.
                const std::uint64_t conv =
                    block.conv_width > 1 ? (block.conv_width - 1) * block.conv_channels : 0;
                unit.state = (conv + ssm_inner * ssm_state) * kStateElementBytes;
            }

            shape.kv_per_token += unit.kv_per_token;
            shape.units.push_back(unit);
        }
        shape.units.push_back(ModelUnit{output_weights, 0, 0});
    } else {
        // A file with metadata but no tensor table. Nothing here can be placed
        // layer by layer, so `units` stays empty and the split falls back to
        // dividing proportionally -- but the cache is still worth knowing, and
        // the header alone is enough for that.
        for (std::uint64_t index = 0; index < layers; ++index) {
            shape.kv_per_token += stated_kv(index);
        }
    }

    shape.known = layers > 0;
    gguf_free(gguf);
    return shape;
}

}  // namespace crucible
