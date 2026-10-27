// SPDX-License-Identifier: MIT
//
// Owning llama.cpp.
//
// Two responsibilities that must not be separated: redirecting llama.cpp's
// logging away from stderr (nobody is reading it there), and enforcing
// the rule that makes Crucible's memory budget work -- an expert nobody is
// using is freed before the next one is loaded, and one somebody is using
// stays, with the next loaded beside it only when there is room. See
// engine/leases.hpp.
//
// One call at a time: the engine holds a lock around every one, whichever of
// its threads makes it, and none of them is the UI's, so the window never
// blocks behind a model load.
#include "crucible/llm/model_host.hpp"

#include "crucible/util/text.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <regex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <ggml-backend.h>
#include <llama.h>
// llama.cpp's staging header, for llama_get_memory_breakdown: the one place it
// says what a load would put on each card. Its own words are "breaking changes
// are allowed", which costs little here: the llama.cpp tag is pinned, so a
// change shows up as a compile error at the upgrade that brings it.
#include <llama-ext.h>

#if defined(CRUCIBLE_HAS_MTMD)
#include <mtmd.h>
#endif

#include "crucible/config/gpu_policy.hpp"
#include "crucible/llm/model_shape.hpp"
#include "crucible/llm/sampling.hpp"
#include "crucible/runtime/devices.hpp"
#include "crucible/runtime/registry.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/resources.hpp"

namespace crucible {
namespace {


// --- llama.cpp logging -----------------------------------------------------
// llama.cpp writes progress and diagnostics to stderr by default. In a
// windowed program that goes nowhere anybody will look, so everything is diverted
// to a file. The callback is global and can fire from llama.cpp's own threads,
// hence the mutex.
std::mutex   g_log_mutex;
std::ofstream g_log_stream;

/// The errors llama.cpp has reported on this thread since a load began: what
/// a failed load says went wrong, which its return value does not. A few,
/// because the useful one -- "unknown model architecture" -- is followed by
/// one that only says the load failed.
thread_local std::vector<std::string> t_errors;

void log_to_file(ggml_log_level level, const char* text, void* /*user_data*/) {
    if (text == nullptr) {
        return;
    }
    if (level == GGML_LOG_LEVEL_ERROR && t_errors.size() < 8) {
        t_errors.emplace_back(text);
    }
    const std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!g_log_stream.is_open()) {
        return;
    }
    const char* tag = "info ";
    switch (level) {
        case GGML_LOG_LEVEL_ERROR: tag = "error"; break;
        case GGML_LOG_LEVEL_WARN:  tag = "warn "; break;
        case GGML_LOG_LEVEL_DEBUG: tag = "debug"; break;
        default: break;
    }
    g_log_stream << tag << " | " << text;
    g_log_stream.flush();
}

/// Why a load failed, in llama.cpp's words: its "error loading model" line
/// when it wrote one, else its first error. Empty when it said nothing.
std::string load_failure() {
    std::string reason;
    for (const std::string& line : t_errors) {
        const std::size_t at = line.find("error loading model: ");
        if (at != std::string::npos) {
            reason = line.substr(at + 21);
            break;
        }
    }
    if (reason.empty() && !t_errors.empty()) {
        reason = t_errors.front();
        if (const std::size_t colon = reason.find(": "); colon != std::string::npos && colon < 48) {
            reason.erase(0, colon + 2);   // the function that said it
        }
    }
    while (!reason.empty() && (reason.back() == '\n' || reason.back() == ' ')) {
        reason.pop_back();
    }
    return reason;
}

/// Bridges llama.cpp's C progress callback to the std::functions the caller
/// gave us: one to report how far along the load is, one to ask whether it
/// should still be happening.
///
/// Returning false from the C callback aborts the load, which is the only way
/// out of one. That mattered more than it looked: a thirty-gigabyte expert is
/// the better part of a minute off the disk, and until this was wired up the
/// answer to "stop" during that minute was that nothing happened -- no token,
/// no error, no way back -- which is indistinguishable from a program that has
/// hung, because for that minute it had.
struct ProgressBridge {
    const ProgressCallback* progress  = nullptr;
    const CancelCallback*   cancel    = nullptr;
    float                   last      = -1.0F;
    bool                    canceled = false;
};

bool progress_trampoline(float progress, void* user_data) {
    auto* bridge = static_cast<ProgressBridge*>(user_data);
    if (bridge == nullptr) {
        return true;
    }
    // Asked first and on every tensor rather than on every whole percent: the
    // percent throttle exists so the screen is not redrawn a thousand times,
    // and a stop that waits for the next percent of a slow disk read is a stop
    // that takes seconds to be noticed.
    //
    // An empty std::function is a legal argument -- "load this, I do not care"
    // -- and calling one throws std::bad_function_call out through llama.cpp's
    // C boundary, where it surfaces as an unexplained "failed to load model".
    if (bridge->cancel != nullptr && *bridge->cancel && (*bridge->cancel)()) {
        bridge->canceled = true;
        return false;
    }
    if (bridge->progress == nullptr || !*bridge->progress) {
        return true;
    }
    // llama.cpp calls this per tensor, which is far more often than a terminal
    // can usefully redraw. Throttle to whole percent.
    if (progress - bridge->last >= 0.01F || progress >= 1.0F) {
        bridge->last = progress;
        (*bridge->progress)(progress);
    }
    return true;
}

llama_split_mode split_mode_from_string(const std::string& name) {
    if (name == "none")   { return LLAMA_SPLIT_MODE_NONE; }
    if (name == "row")    { return LLAMA_SPLIT_MODE_ROW; }
    if (name == "tensor") { return LLAMA_SPLIT_MODE_TENSOR; }
    return LLAMA_SPLIT_MODE_LAYER;
}

/// Bytes, with enough figures that two different amounts do not read the same.
/// "would put 15.5 GB on a card with 15.5 GB free" says nothing.
std::pair<std::string, std::string> two_amounts(std::uint64_t a, std::uint64_t b) {
    std::string first  = format::bytes(a);
    std::string second = format::bytes(b);
    if (first == second) {
        const auto gb = [](std::uint64_t bytes) {
            char text[32];
            std::snprintf(text, sizeof text, "%.2f GB", static_cast<double>(bytes) / (1ULL << 30));
            return std::string(text);
        };
        first  = gb(a);
        second = gb(b);
    }
    return {first, second};
}

/// Will this model fit in the free memory of the cards it would land on?
///
/// Returns an empty string when it fits (or when there is nothing to check --
/// no GPU, or a backend that does not report its memory), and an explanation
/// when it does not.
///
/// The figure is read from the model rather than guessed at. This used to be
/// the file size times a flat 1.25, which refused a 32 GB model on 34.8 GB of
/// free video memory: the allowance invented eight gigabytes of KV cache for a
/// model whose cache is under one. See model_shape.hpp.
///
/// `held` is what Crucible itself already has on the cards, and `holder` names
/// it -- the delegator, almost always. It is not subtracted, because whatever
/// is holding it is staying; it is named in the message, because on a machine
/// where a large expert only just fits, that share is the number the user can
/// actually act on.
std::string vram_shortfall(const std::vector<ComputeDevice>& gpus, const std::string& path,
                           const ModelParams& params, std::uint64_t held, const std::string& holder) {
    const ModelShape shape = read_model_shape(path);
    if (shape.weights == 0) {
        return {};
    }

    if (gpus.empty()) {
        return {};
    }

    // Which cards this model would actually land on, which is not always all
    // of them: "single" mode, and a priority order that gives a card nothing,
    // both leave some out -- and counting memory the model will never be
    // allowed to touch is how a check like this waves through the case it
    // exists to catch.
    //
    // tensor_split is indexed by ggml device index, the same number as
    // ComputeDevice::index, and a zero share means "not used". An empty split
    // is llama.cpp's own default: every GPU, divided by free memory.
    const std::vector<float>& split = params.tensor_split;
    const auto share_of = [&split](const ComputeDevice& gpu) {
        const auto index = static_cast<std::size_t>(gpu.index);
        return index < split.size() ? split[index] : 0.0F;
    };
    const bool everywhere = split.empty();

    // LLAMA_SPLIT_MODE_NONE puts the whole model on main_gpu whatever the
    // split says.
    const bool single = params.split_mode == "none";

    std::vector<const ComputeDevice*> used;
    std::uint64_t available = 0;
    double        weight    = 0.0;   // total of the shares, to normalize by
    for (const ComputeDevice& gpu : gpus) {
        if (single ? gpu.index != params.main_gpu
                   : (!everywhere && share_of(gpu) <= 0.0F)) {
            continue;
        }
        used.push_back(&gpu);
        available += gpu.memory_free;
        // llama.cpp divides by free memory when it is given no split of its
        // own, and by the split when it is. Either way this has to be the same
        // rule, or the two disagree about which card is the tight one.
        weight += everywhere ? static_cast<double>(gpu.memory_free)
                             : static_cast<double>(share_of(gpu));
    }
    if (available == 0 || weight <= 0.0) {
        // A backend that does not report free memory. Nothing to compare
        // against, so let the load proceed rather than refuse on a guess.
        return {};
    }

    // Weights, plus the cache at the context actually configured, plus the
    // compute buffers -- less the input embedding, which llama.cpp keeps in
    // system memory whatever the offload settings say. A model whose header
    // could not be read falls back to a tenth over the file size: small enough
    // not to refuse anything that would have fitted, which is the right way to
    // be wrong when the numbers are unknown.
    const auto cards = static_cast<std::uint64_t>(used.size());
    const std::uint64_t sliding = shape.sliding_cells(params.n_ctx, params.n_batch,
                                                      params.window_cache, params.sequences);
    std::uint64_t divided = static_cast<std::uint64_t>(static_cast<double>(shape.weights) * 1.1);
    std::uint64_t per_card = 0;
    if (shape.known) {
        // What gets spread over the cards, and what every card pays for itself.
        divided  = shape.resident_bytes(params.n_ctx, sliding) - shape.host_weights;
        per_card = shape.compute_bytes(params.n_batch) - shape.logit_bytes(params.n_batch);
    }
    const std::uint64_t needed = divided + per_card * std::max<std::uint64_t>(1, cards)
                               + (shape.known ? shape.logit_bytes(params.n_batch) : 0);

    // --- the tight card ----------------------------------------------------
    //
    // Asked per card, not in aggregate. Adding every card's free memory into
    // one number and comparing the whole model against it is a check that says
    // yes to a model which fits across the machine and overflows one card --
    // and overflowing one card is not a refusal, it is ggml calling abort()
    // with twenty-six gigabytes already uploaded, which takes the process with
    // it and leaves nothing on screen to say why.
    //
    // The reserve is a margin for a card whose free memory is still moving --
    // a compositor redrawing, a browser opening a video -- because the load
    // being checked takes the better part of a minute and the figure it was
    // planned from is stale by the time the weights land.
    //
    // Small on purpose. It was six percent of each card, which on this machine
    // is two and a third gigabytes across three cards, and that refused a model
    // that fits: the 56B needs 8.18 GB of a card with 8.41 free, and six
    // percent left it 7.71. A check that says no to something which would have
    // worked is worse than the abort it exists to prevent -- the abort is rare
    // and loud, the false refusal is quiet and happens every time.
    //
    // So the margin is what a desktop actually moves by, not a fraction of a
    // card, and the planner's own kCardHeadroom sits on top of it for the
    // arrangement it builds.
    const auto reserve_for = [](const ComputeDevice&) {
        return static_cast<std::uint64_t>(128ULL << 20);
    };

    // Layer by layer, where the split says exactly which layers go where.
    //
    // A share of the total is right on average and wrong at the edges: the
    // last layer carries the output weights and is several times the size of
    // the others. Priority mode fills a card to the brim with the real layer
    // sizes, so a check that assumed every layer was average put more on that
    // card than the planner had -- and refused, in priority mode, a model that
    // loaded in even mode on the same three cards.
    std::vector<std::uint64_t> exact;   // bytes per device index, when known
    std::size_t holds_output = split.size();
    if (shape.known && !shape.units.empty() && !everywhere && !single) {
        const std::vector<int> counts = llama_layer_assignment(split, shape.units.size());
        exact.assign(split.size(), 0);
        std::size_t unit = 0;
        for (std::size_t device = 0; device < counts.size(); ++device) {
            for (int i = 0; i < counts[device] && unit < shape.units.size(); ++i, ++unit) {
                exact[device] += shape.unit_bytes(unit, params.n_ctx, sliding);
                holds_output = device;
            }
        }
    }

    for (const ComputeDevice* gpu : used) {
        const double share = everywhere
                                 ? static_cast<double>(gpu->memory_free) / weight
                                 : static_cast<double>(share_of(*gpu)) / weight;
        const auto index = static_cast<std::size_t>(gpu->index);
        const std::uint64_t layers =
            index < exact.size()
                ? exact[index] + (index == holds_output ? shape.logit_bytes(params.n_batch) : 0)
                : static_cast<std::uint64_t>(static_cast<double>(divided) * share);
        const std::uint64_t wants = layers + per_card;
        const std::uint64_t reserve = reserve_for(*gpu);
        const std::uint64_t room    = gpu->memory_free > reserve ? gpu->memory_free - reserve : 0;
        if (wants <= room) {
            continue;
        }
        const std::string where = gpu->description.empty() ? gpu->name : gpu->description;
        const auto [put, free] = two_amounts(wants, gpu->memory_free);
        return "would put " + put + " on " + where + ", which has " + free
             + " free. Lower \"Context\", or turn off \"Use dedicated VRAM only\"";
    }

    if (needed <= available) {
        return {};
    }

    std::string detail;
    if (shape.known && shape.kv_bytes(params.n_ctx, sliding) > 0) {
        // Which part is too big matters: the weights cannot be changed, but
        // the context can, and it is the one number on the settings screen
        // that would make this fit.
        detail = " (" + format::bytes(shape.weights - shape.host_weights) +
                 " of weights plus " + format::bytes(shape.kv_bytes(params.n_ctx, sliding)) +
                 " of context at " + std::to_string(params.n_ctx) + " tokens)";
    }

    std::string where;
    for (const ComputeDevice* gpu : used) {
        if (!where.empty()) {
            where += " + ";
        }
        where += (gpu->description.empty() ? gpu->name : gpu->description);
    }

    std::string advice = ". Lower \"Context\", or turn off \"Use dedicated VRAM only\"";
    if (held > 0 && !holder.empty()) {
        advice = ". " + holder + " holds " + format::bytes(held) + " of it. Lower "
                 "\"Context\", or turn off \"Use dedicated VRAM only\"";
    }

    return "needs about " + format::bytes(needed) + " of video memory" + detail +
           " but only " + format::bytes(available) + " is free on " + where + advice;
}

/// The same, on the cards as they are now.
std::string vram_shortfall(const std::string& path, const ModelParams& params,
                           std::uint64_t held, const std::string& holder) {
    return vram_shortfall(gpu_devices(), path, params, held, holder);
}

/// The projector beside a model file, when it has one: a GGUF in the same
/// folder with "mmproj" in its name -- the way vision models are published,
/// and the way LM Studio lays them out. With several, the one whose name
/// shares the most with the model's.
std::filesystem::path find_projector(const std::string& model_path) {
    const std::filesystem::path model(model_path);
    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    const std::string stem = lower(model.stem().string());
    std::filesystem::path best;
    std::size_t best_shared = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(model.parent_path(), ec)) {
        const std::string name = lower(entry.path().filename().string());
        if (!entry.is_regular_file(ec) || entry.path() == model || name.find("mmproj") == std::string::npos
            || lower(entry.path().extension().string()) != ".gguf") {
            continue;
        }
        // How much of the model's own name is in the projector's, past the
        // "mmproj-" most of them start with.
        std::string bare = name;
        if (const std::size_t at = bare.find("mmproj"); at != std::string::npos) {
            bare.erase(at, 6);
        }
        std::size_t shared = 0;
        for (std::size_t length = std::min(stem.size(), bare.size()); length > 0 && shared == 0; --length) {
            for (std::size_t from = 0; from + length <= stem.size(); ++from) {
                if (bare.find(stem.substr(from, length)) != std::string::npos) {
                    shared = length;
                    break;
                }
            }
        }
        if (best.empty() || shared > best_shared) {
            best        = entry.path();
            best_shared = shared;
        }
    }
    return best;
}

/// Whether a direct-I/O load can be trusted on these devices.
///
/// Not on Vulkan. Direct I/O turns off memory-mapping, and without a mapping
/// llama.cpp uploads the weights through pinned buffers and waits on an event
/// for each one. On the Vulkan backend that wait never returns -- measured on
/// an NVIDIA driver, 2026-10-04: the load stops at whatever percentage the
/// first wait was reached, here 21%, inside
/// ggml_backend_vk_device_event_synchronize, and nothing short of killing the
/// process ends it. No progress callback fires while it waits, so Stop cannot
/// either.
///
/// A mapped load has none of that machinery and is what every other setting
/// already uses. What is given up is only the page-cache saving direct I/O
/// exists for; "Dedicated VRAM only" still refuses a model that will not fit,
/// which is the part of the setting that matters.
bool direct_io_safe(const std::vector<ComputeDevice>& gpus) {
    return std::none_of(gpus.begin(), gpus.end(), [](const ComputeDevice& gpu) {
        return format::to_lower(gpu.backend).find("vulkan") != std::string::npos;
    });
}

int resolve_threads(int configured) {
    if (configured > 0) {
        return configured;
    }
    const unsigned hardware = std::thread::hardware_concurrency();
    // Physical cores generally beat logical ones for GGML; halving is a decent
    // approximation without probing topology.
    return hardware > 1 ? static_cast<int>(hardware / 2) : 1;
}

/// The llama.cpp model parameters a load of `params` uses on `gpus`. `split`
/// is where the tensor split they point at lives, so it has to outlive the
/// load.
llama_model_params model_params_for(const ModelParams& params,
                                    const std::vector<ComputeDevice>& gpus,
                                    std::vector<float>& split) {
    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = params.n_gpu_layers;
    model_params.split_mode   = split_mode_from_string(params.split_mode);
    // Translated at the boundary: Crucible plans in ggml device-index space --
    // what /devices prints and what the priority order names -- and llama.cpp
    // reads an array indexed by position among the GPUs it selected, sized to
    // llama_max_devices(). See llama_tensor_split.
    split = llama_tensor_split(gpus, params.tensor_split);
    if (!split.empty()) {
        model_params.tensor_split = split.data();
    }
    model_params.main_gpu = llama_main_gpu(gpus, params.main_gpu);

    // Keep the weights off the host. `no_host` drops the pinned staging buffer
    // llama.cpp would otherwise keep in RAM for transfers to the card.
    model_params.no_host = params.no_host;
    return model_params;
}

llama_context_params context_params_for(const ModelParams& params) {
    llama_context_params context_params = llama_context_default_params();
    context_params.n_ctx           = static_cast<std::uint32_t>(std::max(0, params.n_ctx));
    context_params.n_batch         = static_cast<std::uint32_t>(std::max(1, params.n_batch));
    context_params.n_threads       = resolve_threads(params.n_threads);
    context_params.n_threads_batch = context_params.n_threads;
    context_params.flash_attn_type = params.flash_attn ? LLAMA_FLASH_ATTN_TYPE_AUTO
                                                       : LLAMA_FLASH_ATTN_TYPE_DISABLED;
    // llama.cpp's default is the whole context on every layer; see
    // ModelParams::window_cache for when it is not.
    context_params.swa_full = !params.window_cache;
    // The delegator's extra sequences share the one cache: each branches off
    // the conversation's and adds a token or two of its own. Kept apart
    // instead, llama.cpp would give each an equal share of the context.
    context_params.n_seq_max  = static_cast<std::uint32_t>(std::max(1, params.sequences));
    context_params.kv_unified = params.sequences > 1;
    return context_params;
}

/// How many labels the delegator scores side by side, each on a sequence of
/// its own: see LoadedModel::score_labels. Sixteen covers most rosters in a
/// single decode. A model with a recurrent state keeps a copy of it for every
/// sequence, so one with a large state gets fewer, to hold those copies under
/// 64 MiB -- and never fewer than one.
int scoring_branches(const ModelParams& params) {
    const std::uint64_t state = read_model_shape(params.path).state_bytes();
    if (state == 0) {
        return 16;
    }
    return static_cast<int>(std::clamp<std::uint64_t>((64ULL << 20) / state, 1, 16));
}

/// What a context has on each card, by ggml device index, in llama.cpp's own
/// figures: what a load would put there, for a dry run's, and what a model
/// holds there, for a loaded one's.
std::vector<CardNeed> cards_of(llama_context* ctx) {
    const std::vector<ggml_backend_dev_t> devices = RuntimeRegistry::devices();
    std::vector<CardNeed> need(devices.size());
    for (const auto& [type, bytes] : llama_get_memory_breakdown(ctx)) {
        if (ggml_backend_buft_is_host(type)) {
            continue;   // system memory, not a card's
        }
        const ggml_backend_dev_t device = ggml_backend_buft_get_device(type);
        for (std::size_t i = 0; i < need.size(); ++i) {
            if (devices[i] == device) {
                need[i].weights += bytes.model;
                need[i].cache   += bytes.context;
                need[i].compute += bytes.compute;
                break;
            }
        }
    }
    return need;
}

/// What a load of `params` would put on each card, by ggml device index, in
/// llama.cpp's own figures.
///
/// A dry run: with no_alloc, llama.cpp places every tensor, sizes the cache
/// and reserves the compute graph exactly as the load will, and allocates
/// none of it. A quarter of a second for a 33 GB model, against the minute
/// the load takes. Empty when the dry run could not be made -- an
/// architecture this llama.cpp does not know, say -- and the real load then
/// says why.
std::vector<CardNeed> measured_need(const ModelParams& params,
                                    const std::vector<ComputeDevice>& gpus) {
    std::vector<float> split;
    llama_model_params model_params = model_params_for(params, gpus, split);
    model_params.no_alloc  = true;
    model_params.load_mode = LLAMA_LOAD_MODE_NONE;
    llama_model* model = llama_model_load_from_file(params.path.c_str(), model_params);
    if (model == nullptr) {
        return {};
    }
    llama_context* ctx = llama_init_from_model(model, context_params_for(params));
    if (ctx == nullptr) {
        llama_model_free(model);
        return {};
    }
    std::vector<CardNeed> need = cards_of(ctx);
    llama_free(ctx);
    llama_model_free(model);
    return need;
}

/// What a backend takes on a card beyond the buffers llama.cpp reports, which
/// is the one thing a dry run cannot see: the pool its kernels draw scratch
/// space from, cuBLAS's workspace, its graphs. Measured on NVIDIA cards,
/// 2026-10-23, through a 1024-token prompt and a reply: Vulkan took 45 MB a
/// card with a 33 GB model, CUDA 14 to 110 MB with gpt-oss-20b and a 1.2B.
/// CUDA's own context, 130 to 160 MB, is not in that: it exists before the
/// free memory is read, so the figure already leaves it out. About twice
/// the worst seen.
std::uint64_t runtime_allowance(const ComputeDevice& gpu) {
    const std::string backend = format::to_lower(gpu.backend);
    if (backend.find("vulkan") != std::string::npos) {
        return 128ULL << 20;
    }
    return 256ULL << 20;   // CUDA, and the ones not measured: Metal, ROCm, SYCL
}

/// vram_shortfall, answered from a dry run instead of from the header.
std::string measured_shortfall(const std::vector<CardNeed>& need,
                               const std::vector<ComputeDevice>& gpus,
                               std::uint64_t held, const std::string& holder) {
    for (const ComputeDevice& gpu : gpus) {
        // A card that reports no memory at all cannot be judged; one that
        // reports none free is full.
        const auto index = static_cast<std::size_t>(gpu.index);
        if (index >= need.size() || need[index].total() == 0 || gpu.memory_total == 0) {
            continue;
        }
        const std::uint64_t wants = need[index].total() + runtime_allowance(gpu);
        // The same margin vram_shortfall keeps for a desktop that moves.
        const std::uint64_t reserve = 128ULL << 20;
        const std::uint64_t room    = gpu.memory_free > reserve ? gpu.memory_free - reserve : 0;
        if (wants <= room) {
            continue;
        }
        const auto [put, free] = two_amounts(wants, gpu.memory_free);
        const std::string where = gpu.description.empty() ? gpu.name : gpu.description;
        std::string out = "would put " + put + " on " + where + ", which has " + free + " free. ";
        if (held > 0 && !holder.empty()) {
            out += holder + " holds " + format::bytes(held) + " of the cards. ";
        }
        return out + "Lower \"Context\", or turn off \"Use dedicated VRAM only\"";
    }
    return {};
}

/// The most any of `gpus` takes beyond the buffers llama.cpp reports.
std::uint64_t runtime_allowance(const std::vector<ComputeDevice>& gpus) {
    std::uint64_t most = 0;
    for (const ComputeDevice& gpu : gpus) {
        most = std::max(most, runtime_allowance(gpu));
    }
    return most;
}

/// Whether the window cache could decide if `params` fits: a model with
/// sliding-window layers, all of it bound for cards that say what they have
/// free. With layers left on the processor that is a different question, and
/// the whole cache stays.
bool window_could_help(const ModelParams& params, const std::vector<ComputeDevice>& gpus) {
    if (params.window_cache) {
        return false;
    }
    const ModelShape shape = read_model_shape(params.path);
    if (!shape.known || shape.window == 0) {
        return false;
    }
    if (params.n_gpu_layers >= 0 && params.n_gpu_layers < static_cast<int>(shape.layers)) {
        return false;
    }
    return std::any_of(gpus.begin(), gpus.end(),
                       [](const ComputeDevice& gpu) { return gpu.memory_total > 0; });
}

/// Whether `params` divides its model between cards by a plan of Crucible's
/// own -- even or priority mode, by layer, over more than one card -- rather
/// than keeping it on one, or leaving it to llama.cpp in auto mode.
bool divided(const ModelParams& params, const GpuConfig& gpu) {
    const GpuSplitMode mode = gpu_split_mode_from_id(gpu.mode);
    if ((mode != GpuSplitMode::Even && mode != GpuSplitMode::Priority) || params.split_mode != "layer") {
        return false;
    }
    return std::count_if(params.tensor_split.begin(), params.tensor_split.end(),
                         [](float share) { return share > 0.0F; }) > 1;
}

/// A line of Crucible's own in the llama.cpp log, where a load's other
/// decisions are.
void note(const std::string& line) {
    log_to_file(GGML_LOG_LEVEL_INFO, ("crucible: " + line + "\n").c_str(), nullptr);
}

/// `params` divided over `cards` as refresh_gpu_split divides it over the
/// cards as they are: for the cards as they would be, with memory given back
/// to them or set aside on them.
ModelParams planned_on(ModelParams params, const GpuConfig& gpu, const std::vector<ComputeDevice>& cards) {
    const GpuSplitMode mode = gpu_split_mode_from_id(gpu.mode);
    if (cards.empty() || mode == GpuSplitMode::Auto) {
        return params;
    }
    const GpuPlan plan = plan_gpu_split(mode, cards, gpu.priority, gpu.main_gpu, model_fit(params));
    if (!plan.split.empty()) {
        params.tensor_split = plan.split;
        params.main_gpu     = gpu.main_gpu;
    }
    return params;
}

/// How a load fits: not at all, with the whole cache, or with the window
/// cache only. See ModelParams::window_cache.
enum class Fit { No, Whole, Window };

/// How a load of `asked` fits `cards`, decided the way ModelHost::load
/// decides it: divided as `gpu` divides it, the whole cache before the window
/// one, and llama.cpp's own figures asked before the header's no is believed.
Fit fits_on(const std::vector<ComputeDevice>& cards, const ModelParams& asked, const GpuConfig& gpu) {
    const ModelParams whole = planned_on(asked, gpu, cards);
    if (vram_shortfall(cards, whole.path, whole, 0, {}).empty()) {
        return Fit::Whole;
    }
    std::vector<CardNeed> need = measured_need(whole, cards);
    if (!need.empty() && measured_shortfall(need, cards, 0, {}).empty()) {
        return Fit::Whole;
    }
    if (!window_could_help(whole, cards)) {
        return Fit::No;
    }
    ModelParams windowed = asked;
    windowed.window_cache = true;
    windowed = planned_on(windowed, gpu, cards);
    if (vram_shortfall(cards, windowed.path, windowed, 0, {}).empty()) {
        return Fit::Window;
    }
    need = measured_need(windowed, cards);
    return !need.empty() && measured_shortfall(need, cards, 0, {}).empty() ? Fit::Window : Fit::No;
}

/// `cards` with the room the delegator `placed` takes set aside: on the card
/// it is pinned to, or shared out as its split shares it. From its header,
/// which is all there is of a model that is not loaded.
std::vector<ComputeDevice> less_delegator(std::vector<ComputeDevice> cards, const ModelParams& placed) {
    const ModelShape shape = read_model_shape(placed.path);
    if (shape.weights == 0) {
        return cards;
    }
    const std::uint64_t sliding =
        shape.sliding_cells(placed.n_ctx, placed.n_batch, placed.window_cache, placed.sequences);
    const std::uint64_t bytes =
        shape.known ? shape.resident_bytes(placed.n_ctx, sliding) - shape.host_weights + shape.compute_bytes(placed.n_batch)
                    : shape.weights + shape.weights / 10;
    const auto share_of = [&placed](const ComputeDevice& card) -> double {
        if (placed.split_mode == "none") {
            return card.index == placed.main_gpu ? 1.0 : 0.0;
        }
        if (placed.tensor_split.empty()) {
            return static_cast<double>(card.memory_free);   // llama.cpp's own rule: by what is free
        }
        const auto index = static_cast<std::size_t>(card.index);
        return index < placed.tensor_split.size() ? static_cast<double>(placed.tensor_split[index]) : 0.0;
    };
    double total = 0.0;
    for (const ComputeDevice& card : cards) {
        total += share_of(card);
    }
    if (total <= 0.0) {
        return cards;
    }
    for (ComputeDevice& card : cards) {
        const auto set_aside = static_cast<std::uint64_t>(static_cast<double>(bytes) * share_of(card) / total);
        card.memory_free = card.memory_free > set_aside ? card.memory_free - set_aside : 0;
    }
    return cards;
}

}  // namespace

int roomiest_context(int n_ctx, int most, std::uint32_t trained, const std::function<bool(int)>& fits) {
    if (trained > 0) {
        most = static_cast<int>(std::min<std::uint64_t>(static_cast<std::uint64_t>(std::max(0, most)), trained));
    }
    // The most first, then three quarters of it and a half, and so on down:
    // a few tries, each a header read and at most two dry runs, rather than
    // a search to the last token for a figure the next load moves anyway.
    // Whole kilotokens, which is how a context is written everywhere else.
    for (int top = most; top > n_ctx; top /= 2) {
        for (const int tried : {top, top / 4 * 3}) {
            const int rounded = tried / 1024 * 1024;
            if (rounded > n_ctx && fits(rounded)) {
                return rounded;
            }
        }
    }
    return n_ctx;
}

// ---------------------------------------------------------------------------
// ModelHost
// ---------------------------------------------------------------------------

ModelHost::ModelHost(std::filesystem::path log_path) {
    {
        std::error_code ec;
        std::filesystem::create_directories(log_path.parent_path(), ec);
        const std::lock_guard<std::mutex> lock(g_log_mutex);
        g_log_stream.open(log_path, std::ios::out | std::ios::trunc);
    }
    // Install the diversion before backend init so even startup chatter about
    // devices and backends goes to the file rather than to a terminal nobody has open.
    llama_log_set(log_to_file, nullptr);

    // And stderr with it, which is not the same diversion and is the one that
    // matters when something goes badly wrong.
    //
    // llama_log_set catches everything ggml *reports*. It does not catch what
    // ggml does when it gives up: GGML_ABORT writes the file, the line and the
    // condition straight to stderr and calls abort(). In a window there is no
    // stderr to read, so a CUDA allocation failing halfway through a
    // thirty-gigabyte upload took the process with it and left a log that
    // simply stopped mid-sentence -- no error, no clue, nothing to act on. The
    // last thing written before the process died is exactly the thing worth
    // having.
    if (std::freopen(log_path.string().c_str(), "a", stderr) != nullptr) {
        std::setvbuf(stderr, nullptr, _IONBF, 0);   // unbuffered: it is a crash log
    }

    // Bring the loadable runtimes in before llama.cpp initializes, so the
    // devices they provide are there from the first model load. A fresh
    // install has none: Crucible ships no backends, and this does nothing until
    // one has been installed -- by the installer, by the window's setup when it
    // starts, or from the settings screen.
    RuntimeRegistry::load_all();

    RuntimeRegistry::init_llama();
}

ModelHost::~ModelHost() {
    experts_.clear();
    router_.reset();
    llama_backend_free();

    const std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_stream.is_open()) {
        g_log_stream.close();
    }
}

std::uint64_t ModelHost::resident_bytes() const {
    std::uint64_t total = 0;
    if (router_) {
        total += router_->bytes();
    }
    for (const Resident& resident : experts_) {
        total += resident.model->bytes();
    }
    return total;
}

std::vector<ModelHost::Held> ModelHost::held() const {
    std::vector<Held> out;
    out.reserve(experts_.size());
    for (const Resident& resident : experts_) {
        out.push_back({resident.id, resident.model.get()});
    }
    return out;
}

std::optional<ExpertId> ModelHost::loaded_expert() const {
    if (experts_.empty()) {
        return std::nullopt;
    }
    return experts_.back().id;
}

std::uint64_t ModelHost::estimate_bytes(const ModelParams& params) {
    std::error_code ec;
    const std::filesystem::path path(params.path);
    if (std::filesystem::is_directory(path, ec)) {
        // An MLX model: its weights are the files in the folder, and what it
        // holds once loaded is about their size and a tenth for the cache.
        std::uint64_t total = 0;
        for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
            if (entry.is_regular_file(ec)) {
                total += entry.file_size(ec);
            }
        }
        return total + total / 10;
    }
    const ModelShape shape = read_model_shape(path);
    if (shape.known) {
        return shape.resident_bytes(params.n_ctx);
    }
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<std::uint64_t>(size) + static_cast<std::uint64_t>(size) / 10;
}

bool ModelHost::fits_beside(const ModelParams& requested, bool beside_mlx) const {
    return fits_beside(requested, Role::Expert, beside_mlx);
}

bool ModelHost::fits_beside(const ModelParams& requested, Role role, bool beside_mlx) const {
    if (experts_.empty() && !router_ && !beside_mlx) {
        return true;   // nothing to share with; the load's own checks speak
    }
    const ModelParams params = placed(requested, role);

    // The cards, where they say what is free. That figure is live, so what is
    // already resident is in it.
    const std::vector<ComputeDevice> gpus = gpu_devices();
    const bool cards_say = std::any_of(gpus.begin(), gpus.end(),
                                       [](const ComputeDevice& gpu) { return gpu.memory_free > 0; });
    if (cards_say
        && !vram_shortfall(params.path, params, resident_bytes(), "the models in use").empty()) {
        // A model with a sliding window is loaded with the window cache when
        // the whole one does not fit (see load), so it fits when that does.
        ModelParams windowed = requested;
        windowed.window_cache = true;
        windowed = placed(windowed, role);
        if (!window_could_help(params, gpus)
            || !vram_shortfall(windowed.path, windowed, resident_bytes(), "the models in use").empty()) {
            return false;
        }
    }

    // The machine's memory, where the cards share it or do not say. A model
    // that would leave less than a tenth of it -- and never under two
    // gigabytes -- is not loaded beside another: the desktop and everything
    // else on it live there too, and a machine paging its model weights is
    // slower than one that waited for the other model to finish.
#if defined(__APPLE__)
    constexpr bool shared = true;
#else
    const bool shared = !cards_say;
#endif
    if (shared) {
        std::uint64_t used  = 0;
        std::uint64_t total = 0;
        if (util::system_memory(used, total) && total > 0) {
            const std::uint64_t available = total > used ? total - used : 0;
            const std::uint64_t headroom  = std::max<std::uint64_t>(2ULL << 30, total / 10);
            if (estimate_bytes(params) + headroom > available) {
                return false;
            }
        }
    }
    return true;
}

bool ModelHost::free_one(const Keep& keep) {
    for (auto it = experts_.begin(); it != experts_.end(); ++it) {
        if (keep && keep(it->params.path)) {
            continue;
        }
        experts_.erase(it);
        return true;
    }
    return false;
}

std::vector<std::string> ModelHost::devices() {
    std::vector<std::string> names;
    for (ggml_backend_dev_t device : RuntimeRegistry::devices()) {
        if (device == nullptr) {
            continue;
        }
        const char* description = ggml_backend_dev_description(device);
        names.emplace_back(description != nullptr ? description : "unknown device");
    }
    return names;
}

ModelParams ModelHost::placed_delegator(const ModelParams& requested) const {
    ModelParams params = requested;
    place_delegator(params, gpu_);
    const std::vector<int> cards = delegator_cards(gpu_);
    if (cards.empty() || vram_shortfall(params.path, params, 0, {}).empty()) {
        return params;  // the mode leaves it alone, or the card it was given holds it
    }
    // The card place_delegator chose is too small for this delegator. Another
    // one may not be: the first that holds it gets it whole.
    for (const int card : cards) {
        ModelParams there = requested;
        pin_to_card(there, card);
        if (vram_shortfall(there.path, there, 0, {}).empty()) {
            return there;
        }
    }
    // No one card holds it, so it is divided the way an expert would be --
    // which in "one card only" leaves it where it was, and the refusal stands.
    ModelParams divided = requested;
    if (refresh_gpu_split(divided, gpu_).empty()) {
        return params;
    }
    return divided;
}

std::unique_ptr<LoadedModel> ModelHost::load(const ModelParams& requested,
                                             Role role,
                                             const ProgressCallback& progress,
                                             const CancelCallback& cancel,
                                             std::string& error) {
    if (requested.path.empty()) {
        error = "no model file configured";
        return nullptr;
    }
    if (!std::filesystem::exists(requested.path)) {
        error = "model file not found: " + requested.path;
        return nullptr;
    }
    // From here to the end, ggml's device lists stay as they are: the split is
    // planned on them, and llama.cpp walks them in each dry run, the load, the
    // context and the projector. A runtime installed meanwhile waits for the
    // load to be over, and a load waits for one being registered.
    const RuntimeRegistry::Reading reading;

    // Ask before llama.cpp does. With no backend registered it throws "no CPU
    // backend found" from somewhere deep in the loader, which is a true
    // statement about ggml and a useless one about what to do next.
    if (RuntimeRegistry::devices().empty()) {
        error = "no runtime installed: install one in Settings, Runtimes";
        return nullptr;
    }

    // Re-plan the split against the memory that is free at this instant.
    //
    // The one stamped on the config was worked out at startup, before the
    // delegator was loaded and before the desktop had done whatever it has
    // since done with the display card. Handing llama.cpp a plan for memory
    // that is no longer there is not a near miss: the load runs to the very
    // last allocation with 32 GB already uploaded and then fails on 48 MB of
    // KV cache. See refresh_gpu_split.
    ModelParams asked = requested;
    if (role == Role::Delegator) {
        asked.sequences = 1 + scoring_branches(asked);
    } else {
        asked = with_room(asked);
    }
    ModelParams params = placed(asked, role);
    const std::vector<ComputeDevice> cards = gpu_devices();

    // What llama.cpp says `params` puts on each card, once a dry run of it has
    // been made. Made only where the header's figures are not enough to go
    // on, and a quarter of a second for a 33 GB model when it is.
    std::vector<CardNeed> need;

    // The cache. llama.cpp keeps the whole context on every layer, sliding-
    // window ones included, because a cache keeping only the window cannot be
    // rewound past it, and reuse_prefix rewinds it every turn. So the whole
    // cache it is, unless that does not fit and the model has a window: then
    // the window cache, 2 GB less on flint at 32K, which reuse_prefix rewinds
    // only as far as it can. Not fitting is asked of the header first and of
    // a dry run before believing it, as the refusal below is.
    if (window_could_help(params, cards) && !vram_shortfall(params.path, params, 0, {}).empty()) {
        need = measured_need(params, cards);
        if (!need.empty() && !measured_shortfall(need, cards, 0, {}).empty()) {
            ModelParams windowed = asked;
            windowed.window_cache = true;
            params = placed(windowed, role);
            need.clear();
            note(std::filesystem::path(params.path).filename().string() + ": the whole cache does not fit at "
                 + std::to_string(params.n_ctx) + " tokens, so the sliding-window layers keep their window only");
        }
    }

    // Layers left to the program, and the processor allowed some: when the
    // whole model does not fit the cards, as many layers as a dry run says
    // fit go on them and the rest on the processor. Without this every layer
    // was asked for, and a model a little larger than the card did not load
    // at all, though Models had said it would run partly on the processor.
    // A layer count at a time, halving: a dry run is a quarter of a second.
    if (!params.gpu_only && params.n_gpu_layers < 0 && !cards.empty()) {
        if (need.empty()) {
            need = measured_need(params, cards);
        }
        const int layers = static_cast<int>(read_model_shape(params.path).layers);
        if (layers > 0 && !need.empty() && !measured_shortfall(need, cards, 0, {}).empty()) {
            // `fits` holds on the cards and `over` does not; one past the
            // repeating layers is the output layer, which the whole asked for.
            int fits = 0;
            int over = layers + 1;
            std::vector<CardNeed> best;
            while (over - fits > 1) {
                ModelParams trial  = params;
                trial.n_gpu_layers = (fits + over) / 2;
                std::vector<CardNeed> got = measured_need(trial, cards);
                if (!got.empty() && measured_shortfall(got, cards, 0, {}).empty()) {
                    fits = trial.n_gpu_layers;
                    best = std::move(got);
                } else {
                    over = trial.n_gpu_layers;
                }
            }
            params.n_gpu_layers = fits;
            need                = std::move(best);
            note(std::filesystem::path(params.path).filename().string() + ": " + std::to_string(fits) + " of "
                 + std::to_string(layers) + " layers on the cards, the rest on the processor");
        }
    }

    // Packed to llama.cpp's own figures. The split above is planned from the
    // header, which knows the weights and the cache and sets 512 MiB aside on
    // every card for the rest -- buffers a dry run of flint put at 194 MiB on
    // a card without the output. Planned again from what a dry run measured,
    // the split packs the cards first in the order closer to full. It is used
    // only when a dry run of it fits every card with the margins the refusal
    // below keeps, so no card is handed more than it holds. Not for a model
    // only partly on the cards: the packing plans every layer.
    if (divided(params, gpu_) && params.n_gpu_layers < 0) {
        const ModelFit fit = model_fit(params);
        if (!fit.units.empty() && need.empty()) {
            need = measured_need(params, cards);
        }
        if (!fit.units.empty() && !need.empty()) {
            ModelParams packed = params;
            refresh_gpu_split(packed, gpu_,
                              calibrated_fit(fit, llama_layer_assignment(params.tensor_split, fit.units.size()),
                                             need, runtime_allowance(cards)));
            if (packed.tensor_split != params.tensor_split) {
                std::vector<CardNeed> checked = measured_need(packed, cards);
                if (!checked.empty() && measured_shortfall(checked, cards, 0, {}).empty()) {
                    params = std::move(packed);
                    need   = std::move(checked);
                }
            }
        }
    }

    // Refuse before allocating anything, when asked to. A driver handed more
    // than the card holds does not fail -- it spills into system RAM and runs
    // on at a crawl -- so this is the only point at which saying no is cheap.
    if (params.vram_only) {
        // What is already on the cards, and what it is. Loading an expert
        // frees every expert nobody is using first, so what is resident then
        // is the delegator and the experts in use -- and loading the delegator
        // after a settings change is the mirror image of that.
        const std::uint64_t held = resident_bytes();
        const std::string   holder =
            role == Role::Expert ? (experts_.empty() ? "the delegator" : "the models in use")
                                 : "the resident experts";
        std::string shortfall = vram_shortfall(params.path, params, held, holder);
        if (!shortfall.empty()) {
            // The estimate allows wide for what the header cannot say, and on a
            // model that only just fits that is the difference: it set 512 MiB
            // aside on every card for buffers a dry run put at 130 to 180 MB,
            // and refused a 33 GB model that three cards held with half a
            // gigabyte to spare on each. So before saying no, ask llama.cpp.
            if (need.empty()) {
                need = measured_need(params, cards);
            }
            if (!need.empty()) {
                shortfall = measured_shortfall(need, cards, held, holder);
            }
        }
        if (!shortfall.empty()) {
            error = std::filesystem::path(params.path).filename().string() + " " + shortfall;
            return nullptr;
        }
    }

    ProgressBridge bridge{&progress, &cancel, -1.0F, false};

    // `split` has to outlive the load call: the parameters point into it.
    const std::vector<ComputeDevice> gpus = gpu_devices();
    std::vector<float>               split;
    llama_model_params model_params = model_params_for(params, gpus, split);
    model_params.progress_callback = progress_trampoline;
    model_params.progress_callback_user_data = &bridge;

    // Direct I/O reads the file straight to the device instead of through the
    // operating system's page cache -- which would otherwise hold a second,
    // full-size copy of every model in RAM long after it was uploaded.
    if (params.direct_io && direct_io_safe(gpus)) {
        model_params.load_mode = LLAMA_LOAD_MODE_DIRECT_IO;
    }

    t_errors.clear();
    llama_model* model = llama_model_load_from_file(params.path.c_str(), model_params);
    if (model == nullptr) {
        // Stopped on purpose is not a failure, and must not be reported as one:
        // "could not load, turn off GPU-only compute" is bad advice to give
        // somebody who pressed stop.
        if (bridge.canceled) {
            error = "stopped";
            return nullptr;
        }
        const std::string said = load_failure();
        const std::string file = std::filesystem::path(params.path).filename().string();
        // A model newer than this llama.cpp is not one that is too large,
        // whatever the settings: saying so sent somebody looking at memory.
        static const std::regex unknown(R"(unknown model architecture: '([^']+)')");
        std::smatch kind;
        if (std::regex_search(said, kind, unknown)) {
            error = file + " is a " + kind[1].str() + " model, which this Crucible's llama.cpp cannot run yet";
            return nullptr;
        }
        error = "llama.cpp could not load " + file + (said.empty() ? std::string() : ": " + said)
              + " (see the Crucible log)";
        // With every layer pinned to the GPU there is no partial offload to
        // fall back on, so a model that no longer fits fails outright rather
        // than quietly running part of itself on the processor -- which is
        // the whole point of the setting, but is worth saying when memory is
        // what llama.cpp ran out of.
        const bool memory = said.empty() || said.find("alloc") != std::string::npos
                         || said.find("memory") != std::string::npos;
        if (params.gpu_only && memory) {
            error += ". Too large for the cards? Turn off \"Keep every layer on the GPU\"";
        }
        return nullptr;
    }

    llama_context* ctx = llama_init_from_model(model, context_params_for(params));
    if (ctx == nullptr) {
        llama_model_free(model);
        error = "could not create a context for " + params.path
              + " (n_ctx " + std::to_string(params.n_ctx) + " may not fit)";
        return nullptr;
    }

    std::unique_ptr<LoadedModel> loaded(new LoadedModel(model, ctx, params.path));
    loaded->window_cache_ = params.window_cache;
    for (const CardNeed& card : cards_of(ctx)) {
        loaded->on_cards_.push_back(card.total());
    }
    if (role == Role::Delegator) {
        // What routing kept the last time this delegator was loaded this way.
        router_key_ = context_key(params);
        if (kept_key_ == router_key_) {
            loaded->kept_ = std::move(kept_routing_);
        }
        kept_routing_.clear();
        kept_key_.clear();
    }
#if defined(CRUCIBLE_HAS_MTMD)
    // A vision model's projector, when there is one beside it: what lets it
    // read the pictures it is sent. A projector that will not load leaves a
    // model that reads text, which is what it was before anyone looked.
    if (role == Role::Expert) {
        if (const std::filesystem::path projector = find_projector(params.path); !projector.empty()) {
            mtmd_context_params vision = mtmd_context_params_default();
            vision.use_gpu       = params.n_gpu_layers != 0;
            vision.n_threads     = resolve_threads(params.n_threads);
            vision.print_timings = false;
            loaded->vision_ = mtmd_init_from_file(projector.string().c_str(), model, vision);
            if (loaded->vision_ != nullptr) {
                loaded->projector_ = projector.string();
            }
        }
    }
#endif
    return loaded;
}

LoadedModel* ModelHost::acquire_router(const ModelParams& params,
                                       const ProgressCallback& progress,
                                       const CancelCallback& cancel,
                                       std::string& error,
                                       const Keep& keep,
                                       bool* crowded,
                                       bool beside_mlx) {
    if (crowded != nullptr) {
        *crowded = false;
    }
    // Kept for an expert's context to leave it room: see with_room.
    router_asked_ = params;
    if (router_ && router_->path() == params.path) {
        return router_.get();
    }

    // Make room, if the delegator needs it.
    //
    // Only reached with "keep delegator loaded" off, where an expert from the
    // previous prompt is still resident when the delegator comes back for the
    // next one. Asking first rather than loading and hoping matters because a
    // driver handed more than the card holds does not fail -- it spills into
    // system RAM, and a delegator running from there takes seconds per
    // decision with nothing on screen to say why.
    //
    // Only experts nobody is using are freed for it. When the ones in use
    // leave no room, the delegator waits: routing goes on keywords meanwhile,
    // which is a worse pick, where the alternative is a crashed agent.
    const ModelParams planned = placed_delegator(params);
    while (!experts_.empty() && !vram_shortfall(planned.path, planned, 0, {}).empty()) {
        if (!free_one(keep)) {
            if (crowded != nullptr) {
                *crowded = true;
            }
            error = "the experts in use leave no room for the delegator";
            return nullptr;
        }
    }

    // An MLX model in use is in a process of its own, where this host can
    // neither see nor free it. The delegator is loaded beside it when both
    // fit, asked the way an expert's load asks, and otherwise not at all:
    // crowded out as it is by experts in use, until the MLX model is let go.
    if (beside_mlx && !fits_beside(params, Role::Delegator, beside_mlx)) {
        if (crowded != nullptr) {
            *crowded = true;
        }
        error = "the models in use leave no room for the delegator";
        return nullptr;
    }

    router_ = load(params, Role::Delegator, progress, cancel, error);
    return router_.get();
}

void ModelHost::release_router() {
    if (router_) {
        kept_key_     = router_key_;
        kept_routing_ = std::move(router_->kept_);
    }
    router_.reset();
}

std::string ModelHost::context_key(const ModelParams& params) {
    // The file as it is on disk, so one replaced under the same name is not
    // taken for the one the prompts were evaluated with.
    std::error_code ec;
    const std::uintmax_t size    = std::filesystem::file_size(params.path, ec);
    const long long      written = static_cast<long long>(
        std::filesystem::last_write_time(params.path, ec).time_since_epoch().count());
    return params.path + '\n' + std::to_string(size) + '\n' + std::to_string(written) + '\n'
         + std::to_string(params.n_ctx) + '\n' + std::to_string(params.n_batch) + '\n'
         + std::to_string(params.sequences) + '\n' + (params.flash_attn ? "flash" : "plain")
         + (params.window_cache ? " window" : " whole");
}

ModelParams ModelHost::placed(const ModelParams& requested, Role role) const {
    if (role == Role::Delegator) {
        return placed_delegator(requested);
    }
    ModelParams params = requested;
    refresh_gpu_split(params, gpu_);
    return params;
}

ModelParams ModelHost::with_room(const ModelParams& asked) const {
    if (asked.n_ctx_most <= asked.n_ctx) {
        return asked;
    }
    // Only a model bound for the cards whole, on cards that say what they
    // have free. With layers left on the processor a longer cache is the
    // processor's memory and its time, which nobody asked to spend.
    const ModelShape shape = read_model_shape(asked.path);
    if (!shape.known || (asked.n_gpu_layers >= 0 && asked.n_gpu_layers < static_cast<int>(shape.layers))) {
        return asked;
    }
    std::vector<ComputeDevice> cards = gpu_devices();
    if (std::none_of(cards.begin(), cards.end(), [](const ComputeDevice& card) { return card.memory_total > 0; })) {
        return asked;
    }
    // The delegator is let go while an expert loads and comes back to route
    // the next task, which, with the agent still holding this model, it can
    // only do beside it. Its room is kept when it had room beside the context
    // asked for: the cache grows into memory nobody would have used, rather
    // than into the delegator's, which would send routing to keywords.
    if (!router_ && !router_asked_.path.empty()) {
        ModelParams delegator = router_asked_;
        delegator.sequences = 1 + scoring_branches(delegator);
        const std::vector<ComputeDevice> spared = less_delegator(cards, placed_delegator(delegator));
        if (fits_on(spared, asked, gpu_) != Fit::No) {
            cards = spared;
        }
    }
    ModelParams grown = asked;
    Fit         fit   = Fit::No;   // how the last context tried fits: the one chosen, when one is
    grown.n_ctx = roomiest_context(asked.n_ctx, asked.n_ctx_most, shape.trained, [&](int n_ctx) {
        ModelParams tried = asked;
        tried.n_ctx = n_ctx;
        fit = fits_on(cards, tried, gpu_);
        return fit != Fit::No;
    });
    if (grown.n_ctx == asked.n_ctx) {
        return asked;
    }
    // The window cache when that is what makes room for the context, or
    // the load, which knows nothing of the delegator's room, would take the
    // whole cache wherever the cards hold it.
    grown.window_cache = fit == Fit::Window;
    note(std::filesystem::path(asked.path).filename().string() + ": a context of " + std::to_string(grown.n_ctx)
         + " tokens rather than " + std::to_string(asked.n_ctx) + ", which the cards hold"
         + (grown.window_cache ? " with the sliding-window layers keeping their window only" : ""));
    return grown;
}

bool ModelHost::could_hold(const ModelParams& params, bool delegator_stays) const {
    std::error_code ec;
    if (params.remote() || params.path.empty() || std::filesystem::is_directory(params.path, ec)) {
        return true;   // nothing of this host's to load: a provider's model, or an MLX one
    }
    const ModelShape shape = read_model_shape(params.path);
    if (shape.weights == 0 || (params.n_gpu_layers >= 0 && params.n_gpu_layers < static_cast<int>(shape.layers))) {
        return true;   // no header to judge by, or set to run partly on the processor: the load says
    }
    // The dry runs below walk ggml's device lists as a load does, and are
    // asked from outside one: held still for them the same way. See load.
    const RuntimeRegistry::Reading reading;
    std::vector<ComputeDevice> cards = gpu_devices();
    if (std::none_of(cards.begin(), cards.end(), [](const ComputeDevice& card) { return card.memory_total > 0; })) {
        return true;
    }
    // Crucible's own models are let go for one that needs the room -- the
    // ones in use when their agents are done with them -- so what they hold
    // counts as free. The delegator does not when it stays loaded.
    const auto give_back = [&cards](const LoadedModel& model) {
        for (ComputeDevice& card : cards) {
            const auto index = static_cast<std::size_t>(card.index);
            if (index < model.on_cards_.size()) {
                card.memory_free = std::min(card.memory_total, card.memory_free + model.on_cards_[index]);
            }
        }
    };
    for (const Resident& resident : experts_) {
        give_back(*resident.model);
    }
    if (router_ && !delegator_stays) {
        give_back(*router_);
    }
    return fits_on(cards, params, gpu_) != Fit::No;
}

/// Would these two be the same loaded model?
///
/// Only the fields that are baked into the model and its context at load time.
/// Sampling is not among them -- temperature and the rest are handed to
/// generate() per call, so two seats that differ only in how they sample can
/// share one loaded model.
bool same_load(const ModelParams& a, const ModelParams& b) {
    return a.path         == b.path
        && a.n_ctx        == b.n_ctx
        && a.n_batch      == b.n_batch
        && a.n_gpu_layers == b.n_gpu_layers
        && a.main_gpu     == b.main_gpu
        && a.split_mode   == b.split_mode
        && a.flash_attn   == b.flash_attn
        && a.tensor_split == b.tensor_split
        && a.no_host      == b.no_host
        && a.direct_io    == b.direct_io;
}

LoadedModel* ModelHost::acquire_expert(const ExpertId& id,
                                       const ModelParams& params,
                                       const ProgressCallback& progress,
                                       const CancelCallback& cancel,
                                       std::string& error,
                                       const Keep& keep,
                                       bool* crowded,
                                       bool beside_mlx) {
    if (crowded != nullptr) {
        *crowded = false;
    }
    // Already resident, under this seat or another. The same weights loaded
    // the same way are one model whichever seat names them: the seats are
    // Crucible's idea, not llama.cpp's, and reloading a file already in memory
    // cost half a minute on every route change for anyone who had not yet
    // found nine different models to fill the table with. Moved to the back,
    // as the one most recently asked for.
    for (auto it = experts_.begin(); it != experts_.end(); ++it) {
        // Except when it was loaded with less room than this load may take
        // and nobody is using it: a chat's model, wanted now by a build's
        // agent, is loaded again with the context the cards hold for it.
        const bool roomier = params.n_ctx_most > it->params.n_ctx_most && !(keep && keep(it->params.path));
        if (it->params.path == params.path && (it->id == id || same_load(it->params, params)) && !roomier) {
            Resident resident = std::move(*it);
            experts_.erase(it);
            resident.id = id;
            experts_.push_back(std::move(resident));
            return experts_.back().model.get();
        }
    }

    // Free first, then load. Holding two at once would double the peak
    // memory and defeat the entire point of loading experts just in time --
    // so every expert nobody is using goes, as it always has.
    release_experts(keep);

    // What is left is in use, and so is an MLX model the caller says is. The
    // new one is loaded beside them when there is room, and otherwise not at
    // all: the caller waits for a lease to end.
    if ((!experts_.empty() || beside_mlx) && !fits_beside(params, beside_mlx)) {
        if (crowded != nullptr) {
            *crowded = true;
        }
        error = "the models in use leave no room for " + std::filesystem::path(params.path).filename().string();
        return nullptr;
    }

    std::unique_ptr<LoadedModel> model = load(params, Role::Expert, progress, cancel, error);
    if (!model) {
        return nullptr;
    }
    experts_.push_back(Resident{id, params, std::move(model)});
    return experts_.back().model.get();
}

void ModelHost::release_experts(const Keep& keep) {
    while (free_one(keep)) {
    }
}

}  // namespace crucible
