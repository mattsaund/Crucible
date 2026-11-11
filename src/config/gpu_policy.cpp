// SPDX-License-Identifier: MIT
//
// Applying the GPU split policy to every model that will be loaded.
#include "crucible/config/gpu_policy.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>

#include "crucible/llm/model_shape.hpp"
#include "crucible/runtime/devices.hpp"

namespace crucible {
namespace {

/// GGUF headers already read during this call.
///
/// Nine experts usually name the same one or two files, and reading a header
/// costs tens of milliseconds; without this, applying a config would spend
/// half a second re-reading the same file.
using ShapeCache = std::map<std::string, ModelShape>;

const ModelShape& shape_of(ShapeCache& cache, const std::string& path) {
    static const ModelShape kNothing;
    if (path.empty()) {
        return kNothing;
    }
    const auto found = cache.find(path);
    if (found != cache.end()) {
        return found->second;
    }
    return cache.emplace(path, read_model_shape(path)).first->second;
}

/// What one model asks of the cards, in the terms the planner works in.
ModelFit fit_of(const ModelShape& shape, const ModelParams& params) {
    ModelFit fit;
    // The input embedding is never offloaded, so it is not part of what the
    // cards are asked to divide. See ModelShape::host_weights.
    const std::uint64_t sliding  = shape.sliding_cells(params.n_ctx, params.n_batch,
                                                       params.window_cache, params.sequences);
    const std::uint64_t resident = shape.resident_bytes(params.n_ctx, sliding);
    fit.resident     = resident > shape.host_weights ? resident - shape.host_weights : resident;
    fit.per_card     = shape.compute_bytes(params.n_batch) - shape.logit_bytes(params.n_batch);
    fit.output_extra = shape.logit_bytes(params.n_batch);

    fit.units.reserve(shape.units.size());
    for (std::size_t i = 0; i < shape.units.size(); ++i) {
        fit.units.push_back(shape.unit_bytes(i, params.n_ctx, sliding));
    }
    return fit;
}

void stamp(ModelParams& params, const std::vector<float>& split, int main_gpu) {
    params.tensor_split  = split;
    params.main_gpu      = main_gpu;
    params.split_planned = true;
}

/// refresh_gpu_split, from `fit` when there is one and from the header when
/// not -- which is only read when the mode plans at all.
std::string replan(ModelParams& params, const GpuConfig& gpu, const ModelFit* fit) {
    const std::vector<ComputeDevice> gpus = gpu_devices();
    if (gpus.empty()) {
        return {};
    }

    const SplitPolicy policy = split_policy(gpu, params, gpus);
    if (policy.mode == GpuSplitMode::Auto) {
        return {};  // an explicit tensor_split in the config file is left alone
    }

    const GpuPlan plan = plan_gpu_split(policy.mode, gpus, policy.order, gpu.main_gpu,
                                        fit != nullptr ? *fit : fit_of(read_model_shape(params.path), params));
    if (plan.split.empty()) {
        return {};
    }
    stamp(params, plan.split, gpu.main_gpu);
    return describe_split(gpus, plan);
}

/// Which card the delegator belongs on. See place_delegator.
int delegator_device(const std::vector<ComputeDevice>& gpus, const SplitPolicy& policy, const GpuConfig& gpu) {
    if (gpus.empty() || policy.mode != GpuSplitMode::Priority) {
        return gpu.main_gpu;
    }
    return apply_priority_order(gpus, policy.order).back().index;
}

}  // namespace

/// Turn the two memory settings into the llama.cpp knobs they mean.
///
/// Applied to every model including the delegator, and applied whatever the
/// split mode is -- "where the compute happens" is a separate question from
/// "how it is divided between cards", and "auto" is an answer to the second
/// one only.
void detail::stamp_memory(ModelParams& params, const GpuConfig& gpu, bool have_gpu, bool direct_io_real) {
    if (have_gpu && gpu.gpu_only) {
        // Negative means every layer plus the output, which is what llama.cpp
        // does with -1 (see llama_model::n_gpu_layers). Overriding the
        // configured count is the point: a number left behind in the config is
        // exactly how a model ends up half on the processor.
        params.n_gpu_layers = -1;
        params.no_host      = true;
        params.gpu_only     = true;
    }
    if (gpu.vram_only) {
        params.no_host   = true;
        // Read past the page cache, so the weights take no RAM on their way
        // to the cards, on Linux, where direct I/O does that. Elsewhere it
        // only turns the mapping off and keeps nothing out of RAM: Windows
        // caches the file all the same, and a Mac would hold the weights
        // twice in its one memory, in the cache and in buffers of their own.
        // There a model is mapped as any other is. What the setting is for,
        // refusing a model that does not fit, is the same everywhere.
        params.direct_io = direct_io_real;
        params.vram_only = true;
    }
}

SplitPolicy split_policy(const GpuConfig& gpu, const ModelParams& params, const std::vector<ComputeDevice>& gpus) {
    SplitPolicy policy{gpu_split_mode_from_id(gpu.mode), gpu.priority};
    if (policy.mode != GpuSplitMode::Auto) {
        return policy;
    }
    // A split written in the config is the person's, and stays theirs.
    const bool written = !params.tensor_split.empty() && !params.split_planned;
    if (!written && links_differ(gpus)) {
        policy.mode = GpuSplitMode::Priority;
        policy.order.clear();   // a priority list kept from another mode is not this one's
    }
    return policy;
}

void place_delegator(ModelParams& params, const GpuConfig& gpu) {
    const std::vector<ComputeDevice> gpus = gpu_devices();
    const SplitPolicy policy = split_policy(gpu, params, gpus);
    if (policy.mode == GpuSplitMode::Auto) {
        // "Auto" is the user saying llama.cpp decides how models are divided.
        // Pinning the delegator would be Crucible overriding that for one model
        // and not the other, which is exactly the kind of hidden policy the
        // mode exists to switch off -- where the cards' links are alike. See
        // split_policy for where they are not.
        return;
    }
    if (gpus.size() < 2) {
        return;  // nothing to choose between
    }
    const int device = delegator_device(gpus, policy, gpu);
    std::vector<float> split(static_cast<std::size_t>(gpus.back().index) + 1, 0.0F);
    split[static_cast<std::size_t>(device)] = 1.0F;
    stamp(params, split, device);
}

std::vector<int> delegator_cards(const GpuConfig& gpu, const ModelParams& params) {
    const std::vector<ComputeDevice> gpus = gpu_devices();
    const SplitPolicy policy = split_policy(gpu, params, gpus);
    if (policy.mode == GpuSplitMode::Auto || gpus.size() < 2) {
        return {};
    }
    const int first = delegator_device(gpus, policy, gpu);
    std::vector<int> cards{first};
    if (policy.mode == GpuSplitMode::Single) {
        return cards;
    }
    std::vector<ComputeDevice> rest;
    for (const ComputeDevice& card : gpus) {
        if (card.index != first) {
            rest.push_back(card);
        }
    }
    std::stable_sort(rest.begin(), rest.end(), [](const ComputeDevice& a, const ComputeDevice& b) {
        return a.memory_free > b.memory_free;
    });
    for (const ComputeDevice& card : rest) {
        cards.push_back(card.index);
    }
    return cards;
}

void pin_to_card(ModelParams& params, int device) {
    const std::vector<ComputeDevice> gpus = gpu_devices();
    if (gpus.empty() || device < 0) {
        return;
    }
    std::vector<float> split(static_cast<std::size_t>(gpus.back().index) + 1, 0.0F);
    if (static_cast<std::size_t>(device) >= split.size()) {
        return;
    }
    split[static_cast<std::size_t>(device)] = 1.0F;
    stamp(params, split, device);
}

std::string refresh_gpu_split(ModelParams& params, const GpuConfig& gpu) {
    return replan(params, gpu, nullptr);
}

std::string refresh_gpu_split(ModelParams& params, const GpuConfig& gpu, const ModelFit& fit) {
    return replan(params, gpu, &fit);
}

ModelFit model_fit(const ModelParams& params) {
    return fit_of(read_model_shape(params.path), params);
}

std::string apply_gpu_policy(Config& config) {
    const std::vector<ComputeDevice> all_gpus = gpu_devices();
    const bool have_gpu = !all_gpus.empty();

    // Throw away any card in the priority order that is not in the machine.
    //
    // A stale entry is not harmless. apply_priority_order drops it when it
    // builds the ranking -- so the order still works -- but the stored list
    // goes on naming it, and the settings screen goes on drawing a list that
    // does not match what is written down. Worse, it is silent: an order of
    // "1, 2, 3" on a three-card machine numbered 0, 1, 2 means "card 1 first,
    // then card 2, then card 0", which is not what anybody typing 1, 2, 3
    // meant. Repairing it here, where the device list finally exists, is the
    // first moment it can be done at all -- load_config runs before any
    // runtime has been loaded, so it has no cards to check against.
    if (have_gpu && !config.gpu.priority.empty()) {
        std::vector<int> kept;
        for (const int index : config.gpu.priority) {
            const bool real = std::any_of(all_gpus.begin(), all_gpus.end(),
                                          [index](const ComputeDevice& gpu) {
                                              return gpu.index == index;
                                          });
            const bool already =
                std::find(kept.begin(), kept.end(), index) != kept.end();
            if (real && !already) {
                kept.push_back(index);
            }
        }
        config.gpu.priority = std::move(kept);
    }

    detail::stamp_memory(config.router, config.gpu, have_gpu, kDirectIoReal);
    detail::stamp_memory(config.defaults, config.gpu, have_gpu, kDirectIoReal);
    for (auto& [id, expert] : config.experts) {
        detail::stamp_memory(expert, config.gpu, have_gpu, kDirectIoReal);
    }

    // "Auto" plans nothing, except over cards whose links differ, where each
    // model without a split of its own is planned fastest link first. See
    // split_policy.
    const GpuSplitMode mode = gpu_split_mode_from_id(config.gpu.mode);
    const std::vector<ComputeDevice>& gpus = all_gpus;
    if (mode == GpuSplitMode::Auto && !links_differ(gpus)) {
        return {};
    }
    if (gpus.size() < 2 && mode != GpuSplitMode::Single) {
        // Nothing to divide. Saying so is better than silently writing a
        // one-element split that looks like it did something.
        return {};
    }

    // The delegator gets one card of its own. See place_delegator.
    place_delegator(config.router, config.gpu);

    // A split is per-model, not per-machine. Priority mode fills the cards in
    // order, and how far down the order a model reaches depends on how big it
    // is -- a 1B expert never leaves the first card, a 30B one uses all three.
    // So each seat gets the split computed for the file it actually names.
    //
    // What is stamped here is a plan made from the memory that is free right
    // now, which is not the memory that will be free when the model is loaded:
    // the delegator has yet to be loaded, and a desktop's video memory moves
    // about by gigabytes. So this is the preview the settings screen shows and
    // the starting point for a load, and ModelHost re-plans against live
    // memory immediately before it commits. See refresh_gpu_split.
    ShapeCache shapes;
    const auto plan_for = [&](const ModelParams& params) {
        const SplitPolicy policy = split_policy(config.gpu, params, gpus);
        if (policy.mode == GpuSplitMode::Auto) {
            return GpuPlan{};   // its own split, written in the config
        }
        const ModelShape& shape = shape_of(shapes, params.path);
        return plan_gpu_split(policy.mode, gpus, policy.order, config.gpu.main_gpu, fit_of(shape, params));
    };

    GpuPlan       described;
    std::uint64_t described_bytes = 0;
    std::string   described_model;
    for (auto& [id, expert] : config.experts) {
        const GpuPlan plan = plan_for(expert);
        if (plan.split.empty()) {
            continue;
        }
        stamp(expert, plan.split, config.gpu.main_gpu);
        // Keep the largest expert's arrangement: it is the one worth reporting,
        // the one most likely not to fit, and the sensible thing to hand to
        // `defaults` below.
        if (const std::uint64_t bytes = shape_of(shapes, expert.path).weights;
            described.split.empty() || bytes > described_bytes) {
            described       = plan;
            described_bytes = bytes;
            described_model = expert.model;
        }
    }

    // `defaults` names no file of its own, so there is no model to fill the
    // cards with and no split that is really "its". It still has to be stamped
    // -- a hand-written tensor_split left in the config file would otherwise
    // survive a mode that is supposed to override it -- so it takes the
    // largest expert's, or a capacity-proportional one when no seat is filled.
    // Not in "auto", which is the mode that keeps a hand-written one.
    const GpuPlan defaults_plan = mode == GpuSplitMode::Auto ? GpuPlan{}
                                : described.split.empty()    ? plan_for(config.defaults)
                                                             : described;
    if (!defaults_plan.split.empty()) {
        stamp(config.defaults, defaults_plan.split, config.gpu.main_gpu);
    }
    if (described.split.empty()) {
        described = defaults_plan;
    }

    if (described.split.empty()) {
        return {};
    }
    // Name the model. The split is worked out per-model now, so a bare list of
    // percentages would be a fact about something the reader has to guess at.
    const std::string layout = describe_split(gpus, described);
    return described_model.empty() ? layout : described_model + ": " + layout;
}

}  // namespace crucible
