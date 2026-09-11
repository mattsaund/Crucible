// SPDX-License-Identifier: MIT
//
// Device enumeration and the GPU split policy. See devices.hpp for the why.
#include "crucible/runtime/devices.hpp"

#include <algorithm>
#include <numeric>
#include <utility>

#include <ggml-backend.h>
#include <llama.h>

#include "crucible/runtime/backend.hpp"
#include "crucible/util/format.hpp"

namespace crucible {

std::uint64_t usable_memory(const ComputeDevice& gpu, std::uint64_t reserve) {
    // Free when the backend reports it, total when it does not. A backend that
    // reports neither gets nothing, and the caller falls back to an equal
    // share rather than dividing by zero.
    const std::uint64_t bytes = gpu.memory_free > 0 ? gpu.memory_free : gpu.memory_total;
    return bytes > reserve ? bytes - reserve : 0;
}

std::string ComputeDevice::label() const {
    std::string text = description.empty() ? name : description;
    if (memory_total > 0) {
        // Free as well as total. Whether a model loads is decided by what is
        // free right now, not by what is on the box, and the difference on a
        // card that is also driving a desktop is several gigabytes.
        text += " (" + format::bytes(memory_free) + " free of " +
                format::bytes(memory_total) + ")";
    }
    return text;
}

std::vector<ComputeDevice> compute_devices() {
    std::vector<ComputeDevice> devices;
    const std::size_t count = ggml_backend_dev_count();
    devices.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        ggml_backend_dev_t handle = ggml_backend_dev_get(i);
        if (handle == nullptr) {
            continue;
        }

        ComputeDevice device;
        device.index = static_cast<int>(i);
        if (const char* name = ggml_backend_dev_name(handle); name != nullptr) {
            device.name = name;
        }
        if (const char* description = ggml_backend_dev_description(handle); description != nullptr) {
            device.description = description;
        }
        if (ggml_backend_reg_t reg = ggml_backend_dev_backend_reg(handle); reg != nullptr) {
            if (const char* reg_name = ggml_backend_reg_name(reg); reg_name != nullptr) {
                device.backend = reg_name;
            }
        }

        std::size_t free_bytes = 0;
        std::size_t total_bytes = 0;
        ggml_backend_dev_memory(handle, &free_bytes, &total_bytes);
        device.memory_free  = free_bytes;
        device.memory_total = total_bytes;

        device.is_gpu = ggml_backend_dev_type(handle) == GGML_BACKEND_DEVICE_TYPE_GPU;
        devices.push_back(std::move(device));
    }
    return devices;
}

std::vector<ComputeDevice> gpu_devices() {
    std::vector<ComputeDevice> gpus;
    for (ComputeDevice& device : compute_devices()) {
        if (!device.is_gpu) {
            continue;
        }
        // De-duplicated by description, the way llama.cpp de-duplicates by the
        // driver's device id. Install CUDA and Vulkan together and every NVIDIA
        // card is reported twice; llama.cpp keeps the first of each, and a
        // split planned against a list it does not share would give every card
        // the share meant for its neighbor.
        const bool seen = std::any_of(gpus.begin(), gpus.end(),
                                      [&device](const ComputeDevice& other) {
                                          return !device.description.empty() &&
                                                 other.description == device.description;
                                      });
        if (!seen) {
            gpus.push_back(std::move(device));
        }
    }
    return gpus;
}

std::vector<float> llama_tensor_split(const std::vector<ComputeDevice>& gpus,
                                      const std::vector<float>& split) {
    if (split.empty()) {
        return {};  // llama.cpp's own default: divide across every GPU
    }
    // Sized to what llama.cpp reads, not to what we planned. It copies
    // llama_max_devices() floats out of this pointer, and a shorter buffer is
    // read past its end.
    std::vector<float> out(static_cast<std::size_t>(llama_max_devices()), 0.0F);
    for (std::size_t slot = 0; slot < gpus.size() && slot < out.size(); ++slot) {
        const auto index = static_cast<std::size_t>(gpus[slot].index);
        out[slot] = index < split.size() ? split[index] : 0.0F;
    }
    return out;
}

int llama_main_gpu(const std::vector<ComputeDevice>& gpus, int main_gpu) {
    for (std::size_t slot = 0; slot < gpus.size(); ++slot) {
        if (gpus[slot].index == main_gpu) {
            return static_cast<int>(slot);
        }
    }
    // A main GPU that is not on the list any more. Zero is the first card,
    // which is what llama.cpp would have used anyway, and is in range -- an
    // out-of-range value makes it refuse the load.
    return 0;
}

std::string_view gpu_split_mode_id(GpuSplitMode mode) {
    switch (mode) {
        case GpuSplitMode::Auto:     return "auto";
        case GpuSplitMode::Even:     return "even";
        case GpuSplitMode::Priority: return "priority";
        case GpuSplitMode::Single:   return "single";
    }
    return "auto";
}

GpuSplitMode gpu_split_mode_from_id(std::string_view id) {
    if (id == "even")     { return GpuSplitMode::Even; }
    if (id == "priority") { return GpuSplitMode::Priority; }
    if (id == "single")   { return GpuSplitMode::Single; }
    return GpuSplitMode::Auto;
}

std::vector<ComputeDevice> apply_priority_order(const std::vector<ComputeDevice>& gpus,
                                                const std::vector<int>& order) {
    std::vector<ComputeDevice> arranged;
    arranged.reserve(gpus.size());

    const auto already_taken = [&arranged](int index) {
        return std::any_of(arranged.begin(), arranged.end(),
                           [index](const ComputeDevice& gpu) { return gpu.index == index; });
    };

    for (const int index : order) {
        const auto found = std::find_if(gpus.begin(), gpus.end(),
                                        [index](const ComputeDevice& gpu) {
                                            return gpu.index == index;
                                        });
        // An index listed twice would take two places in the order, and the
        // second one would silently push a real card down the list.
        if (found != gpus.end() && !already_taken(index)) {
            arranged.push_back(*found);
        }
    }

    for (const ComputeDevice& gpu : gpus) {
        if (!already_taken(gpu.index)) {
            arranged.push_back(gpu);
        }
    }
    return arranged;
}

namespace {

/// The devices `targets` gives anything to, in ggml index order.
///
/// The order matters and is not the priority order: llama.cpp walks the layers
/// from zero and the devices by index, so index order is the order layers are
/// handed out in. Priority decides how much each card gets, never which layers.
std::vector<const ComputeDevice*> receiving(const std::vector<ComputeDevice>& gpus,
                                           const std::vector<std::uint64_t>& targets) {
    std::vector<const ComputeDevice*> devices;
    for (const ComputeDevice& gpu : gpus) {
        const auto index = static_cast<std::size_t>(gpu.index);
        if (index < targets.size() && targets[index] > 0) {
            devices.push_back(&gpu);
        }
    }
    std::sort(devices.begin(), devices.end(),
              [](const ComputeDevice* a, const ComputeDevice* b) { return a->index < b->index; });
    return devices;
}

/// What each device ends up holding, for a given set of counts.
///
/// Units are handed out in order and devices are walked by index, so a count
/// vector is all it takes to know which units land where.
std::vector<std::uint64_t> usage_of(const std::vector<int>& counts,
                                    const std::vector<std::uint64_t>& units) {
    std::vector<std::uint64_t> used(counts.size(), 0);
    std::size_t unit = 0;
    for (std::size_t device = 0; device < counts.size(); ++device) {
        for (int i = 0; i < counts[device] && unit < units.size(); ++i, ++unit) {
            used[device] += units[unit];
        }
    }
    return used;
}

/// Move units off any card that is over its capacity.
///
/// The greedy pass has to put every unit somewhere -- llama.cpp's split always
/// normalizes to one, so the last card in index order receives whatever the
/// others declined whether it has room or not. On a machine where the model
/// only just fits, that is exactly what happens: each earlier card stops one
/// unit short of its limit and the last card inherits all of the slack at once.
///
/// So the greedy pass is followed by this, which only ever moves a unit from a
/// card that is over its capacity to one with room for it. It stops when
/// nothing is over, or when no card can take what is left -- which means the
/// model does not fit, and saying so is the VRAM check's job, not this one's.
void rebalance(std::vector<int>& counts,
               const std::vector<std::uint64_t>& units,
               const std::vector<std::uint64_t>& capacity) {
    for (std::size_t guard = 0; guard <= units.size(); ++guard) {
        const std::vector<std::uint64_t> used = usage_of(counts, units);

        std::size_t   from = counts.size();
        std::uint64_t worst = 0;
        for (std::size_t device = 0; device < counts.size(); ++device) {
            if (counts[device] > 0 && used[device] > capacity[device] &&
                used[device] - capacity[device] > worst) {
                worst = used[device] - capacity[device];
                from  = device;
            }
        }
        if (from == counts.size()) {
            return;  // everything fits
        }

        // The unit that would move is the last one in that card's range.
        std::size_t end = 0;
        for (std::size_t device = 0; device <= from; ++device) {
            end += static_cast<std::size_t>(counts[device]);
        }
        const std::uint64_t moving = units[std::min(end, units.size()) - 1];

        std::size_t   into = counts.size();
        std::uint64_t best = 0;
        for (std::size_t device = 0; device < counts.size(); ++device) {
            if (device == from || capacity[device] <= used[device]) {
                continue;
            }
            const std::uint64_t room = capacity[device] - used[device];
            if (room >= moving && room > best) {
                best = room;
                into = device;
            }
        }
        if (into == counts.size()) {
            return;  // nowhere it would fit; the model is too large for the machine
        }

        --counts[from];
        ++counts[into];
    }
}

/// Hand out whole units against per-card byte targets.
///
/// Returns the count for each device, indexed by ggml device index. The last
/// card in index order takes whatever is left, and `rebalance` then moves back
/// anything that leaves a card over its capacity.
std::vector<int> place_units(const std::vector<ComputeDevice>& gpus,
                             const std::vector<std::uint64_t>& targets,
                             const std::vector<std::uint64_t>& capacity,
                             const std::vector<std::uint64_t>& units,
                             std::size_t width) {
    std::vector<int> counts(width, 0);
    const std::vector<const ComputeDevice*> devices = receiving(gpus, targets);
    if (devices.empty()) {
        return counts;
    }

    std::size_t unit = 0;
    for (std::size_t i = 0; i < devices.size(); ++i) {
        const auto index = static_cast<std::size_t>(devices[i]->index);
        const bool last  = i + 1 == devices.size();
        std::uint64_t budget = targets[index];
        while (unit < units.size() && (last || units[unit] <= budget)) {
            if (!last) {
                budget -= units[unit];
            }
            ++counts[index];
            ++unit;
        }
    }

    // Capacity is the only correction there is room for.
    //
    // It is tempting to add a second pass that nudges units towards the byte
    // targets as well, and it would be dead code: in priority mode every card
    // that receives anything is filled to its capacity, so the slack it leaves
    // behind is by construction less than one unit and there is never room for
    // a whole one to move back. The last card in index order inherits that
    // slack -- at most one unit per earlier card -- and nothing can be done
    // about it without splitting a layer.
    rebalance(counts, units, capacity);
    return counts;
}

/// Turn unit counts into the `tensor_split` that produces exactly them.
///
/// llama.cpp makes the split cumulative, normalizes it, and sends unit `i` to
/// the first device whose cumulative share exceeds `i / total`. Feeding it the
/// counts directly would put every boundary exactly on a comparison it is about
/// to make in single precision, so each boundary is nudged half a unit clear of
/// one: the first device asks for half a unit less and the last for half a unit
/// more, which leaves every share in the middle of its interval and the total
/// unchanged.
std::vector<float> split_from_counts(const std::vector<int>& counts) {
    std::size_t first = counts.size();
    std::size_t last  = 0;
    int total = 0;
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] > 0) {
            first = std::min(first, i);
            last  = i;
            total += counts[i];
        }
    }
    if (total == 0) {
        return {};
    }

    std::vector<float> split(counts.size(), 0.0F);
    for (std::size_t i = 0; i < counts.size(); ++i) {
        split[i] = static_cast<float>(counts[i]);
    }
    if (first != last) {
        split[first] -= 0.5F;
        split[last]  += 0.5F;
    }
    for (float& share : split) {
        share /= static_cast<float>(total);
    }
    return split;
}

/// Normalize a set of weights into a split, or return nothing if they are all
/// zero.
std::vector<float> normalized(std::vector<float> weights) {
    const float total = std::accumulate(weights.begin(), weights.end(), 0.0F);
    if (total <= 0.0F) {
        return {};
    }
    for (float& share : weights) {
        share /= total;
    }
    return weights;
}

}  // namespace

GpuPlan plan_gpu_split(GpuSplitMode mode,
                       const std::vector<ComputeDevice>& gpus,
                       const std::vector<int>& order,
                       int main_gpu,
                       const ModelFit& fit) {
    // With one GPU there is nothing to divide, and an explicit split would
    // only be a way to get it wrong.
    if (gpus.size() < 2 || mode == GpuSplitMode::Auto) {
        return {};
    }

    // tensor_split is indexed by ggml device index, not by position in the GPU
    // list, so the vector has to be long enough to reach the highest one.
    std::size_t width = 0;
    for (const ComputeDevice& gpu : gpus) {
        width = std::max(width, static_cast<std::size_t>(gpu.index) + 1);
    }

    if (mode == GpuSplitMode::Single) {
        const auto chosen = std::find_if(gpus.begin(), gpus.end(),
                                         [main_gpu](const ComputeDevice& gpu) {
                                             return gpu.index == main_gpu;
                                         });
        const int index = chosen != gpus.end() ? chosen->index : gpus.front().index;
        GpuPlan plan;
        plan.split.assign(width, 0.0F);
        plan.split[static_cast<std::size_t>(index)] = 1.0F;
        plan.units.assign(width, 0);
        plan.units[static_cast<std::size_t>(index)] = static_cast<int>(fit.units.size());
        return plan;
    }

    // How many bytes of the model each card is meant to end up with. Priority
    // fills them in the stated order; even mode divides by capacity. Either way
    // this is the intent, and the unit walk below is what turns intent into the
    // whole layers llama.cpp actually places.
    const std::vector<ComputeDevice> ranked =
        mode == GpuSplitMode::Priority ? apply_priority_order(gpus, order) : gpus;

    // The card holding the output unit pays for the logits buffer on top of
    // everything else. Which card that is depends on the targets, and the
    // targets depend on the reserve -- so the first pass leaves the logits out,
    // and the second charges them to the card the first pass landed them on.
    // Two passes because one is wrong and three would not change the answer.
    std::size_t output_device = width;  // width == "nobody yet"
    std::vector<std::uint64_t> targets;
    std::vector<std::uint64_t> capacity;

    for (int pass = 0; pass < 2; ++pass) {
        targets.assign(width, 0);
        capacity.assign(width, 0);
        const auto reserve_for = [&](const ComputeDevice& gpu) {
            const auto index = static_cast<std::size_t>(gpu.index);
            return fit.per_card + kCardHeadroom +
                   (index == output_device ? fit.output_extra : 0);
        };

        if (mode == GpuSplitMode::Even) {
            // "Even" means even in work, not even in count: a 16 GB card should
            // take twice the layers of an 8 GB one, or the small card runs out
            // first and the split fails for a model that would otherwise fit.
            for (const ComputeDevice& gpu : ranked) {
                targets[static_cast<std::size_t>(gpu.index)] = usable_memory(gpu, reserve_for(gpu));
            }
        } else {
            // Priority: fill each card in the stated order up to what it can
            // actually hold, and give the next card only what is left over.
            // That is what "priority" means, and it is the one arrangement that
            // keeps a model whole on the fastest card when it fits there.
            //
            // The order comes from apply_priority_order, the same rule the GPU
            // priority screen lays its list out with: a card the config names
            // that is no longer plugged in is dropped, one that has appeared
            // since lands at the end.
            std::uint64_t remaining = fit.resident;
            for (const ComputeDevice& gpu : ranked) {
                const std::uint64_t take = std::min(usable_memory(gpu, reserve_for(gpu)), remaining);
                targets[static_cast<std::size_t>(gpu.index)] = take;
                remaining -= take;
            }
            if (fit.resident == 0 || remaining > 0) {
                // Either the size is unknown, or the model does not fit in the
                // cards at all. Divide by capacity: it will not load either
                // way, but a split proportional to what each card holds is the
                // arrangement that gets closest, and it never hands a card more
                // than it can hold. "Dedicated VRAM only" is what turns the
                // second case into a message rather than a driver quietly
                // spilling into system RAM.
                for (const ComputeDevice& gpu : ranked) {
                    targets[static_cast<std::size_t>(gpu.index)] = usable_memory(gpu, reserve_for(gpu));
                }
            }
        }

        for (const ComputeDevice& gpu : gpus) {
            capacity[static_cast<std::size_t>(gpu.index)] = usable_memory(gpu, reserve_for(gpu));
        }

        const std::vector<const ComputeDevice*> devices = receiving(gpus, targets);
        const std::size_t landed = devices.empty()
                                       ? width
                                       : static_cast<std::size_t>(devices.back()->index);
        if (landed == output_device) {
            break;  // the second pass would ask the same question again
        }
        output_device = landed;
    }

    GpuPlan plan;

    // Nothing to plan against: a backend that reports neither free nor total
    // memory, or cards so small that the headroom alone exhausts them. An equal
    // share is the only honest answer, and it is better than no opinion at all,
    // which would leave llama.cpp dividing by a free-memory figure the same
    // backend just declined to give.
    if (std::all_of(targets.begin(), targets.end(),
                    [](std::uint64_t bytes) { return bytes == 0; })) {
        std::vector<float> equal(width, 0.0F);
        for (const ComputeDevice& gpu : gpus) {
            equal[static_cast<std::size_t>(gpu.index)] = 1.0F;
        }
        plan.split = normalized(std::move(equal));
        return plan;
    }

    if (!fit.units.empty()) {
        plan.units = place_units(gpus, targets, capacity, fit.units, width);
        plan.split = split_from_counts(plan.units);
        if (!plan.split.empty()) {
            return plan;
        }
        plan.units.clear();
    }

    // No unit list -- an unreadable header, or a caller that did not supply
    // one. Fall back to sharing out the byte targets proportionally, which is
    // what this did before layer-exact placement and is still right to within
    // one layer.
    std::vector<float> weights(width, 0.0F);
    for (std::size_t i = 0; i < width; ++i) {
        weights[i] = static_cast<float>(targets[i]);
    }
    plan.split = normalized(std::move(weights));
    return plan;
}

std::vector<float> compute_tensor_split(GpuSplitMode mode,
                                        const std::vector<ComputeDevice>& gpus,
                                        const std::vector<int>& order,
                                        int main_gpu,
                                        const ModelFit& fit) {
    return plan_gpu_split(mode, gpus, order, main_gpu, fit).split;
}

GpuSettingSupport gpu_setting_support(const std::vector<ComputeDevice>& devices) {
    std::size_t gpus           = 0;
    bool        reports_memory = false;
    bool        can_split      = false;
    std::string backend;

    for (const ComputeDevice& device : devices) {
        if (!device.is_gpu) {
            continue;
        }
        ++gpus;
        reports_memory = reports_memory || device.memory_total > 0;
        if (const auto kind = backend_from_reg_name(device.backend)) {
            can_split = can_split || backend_info(*kind).multi_device;
            backend   = backend_info(*kind).name;
        } else if (backend.empty()) {
            // A backend Crucible does not manage still has a name worth using.
            backend = device.backend;
        }
    }

    GpuSettingSupport support;
    if (gpus == 0) {
        // Everything about the GPU is moot, and the fix is the same for all of
        // them, so it is said the same way.
        support.split = support.gpu_only = support.vram_only =
            "no GPU runtime is loaded -- install one under Runtimes";
        return support;
    }

    if (gpus == 1) {
        support.split = "only one GPU: " + backend + " has nothing to divide";
    } else if (!can_split) {
        support.split = backend + " runs one device at a time";
    }
    if (!reports_memory) {
        // The check compares a model's size against free device memory. A
        // backend that reports none turns the setting into one that saves and
        // then does nothing at all.
        support.vram_only = backend + " does not report how much memory its devices have";
    }
    return support;
}

std::string describe_split(const std::vector<ComputeDevice>& gpus, const GpuPlan& plan) {
    if (plan.split.empty()) {
        return "llama.cpp decides";
    }

    int total_units = 0;
    for (const int count : plan.units) {
        total_units += count;
    }

    std::string text;
    for (const ComputeDevice& gpu : gpus) {
        const auto index = static_cast<std::size_t>(gpu.index);
        if (index >= plan.split.size() || plan.split[index] <= 0.0F) {
            continue;
        }
        if (!text.empty()) {
            text += ", ";
        }
        const std::string name = gpu.description.empty() ? gpu.name : gpu.description;
        if (total_units > 0) {
            text += name + " " + std::to_string(plan.units[index]) + "/" +
                    std::to_string(total_units) + " layers";
        } else {
            text += name + " " +
                    format::number(static_cast<double>(plan.split[index]) * 100.0, 0) + "%";
        }
    }
    return text.empty() ? "llama.cpp decides" : text;
}

}  // namespace crucible
