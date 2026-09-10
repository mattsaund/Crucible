// SPDX-License-Identifier: MIT
//
// The compute devices ggml found, and how a model is spread across them.
//
// A machine with three GPUs can hold a far larger expert than any one of them,
// but only if the layers are divided sensibly. "Sensibly" is not one rule:
// three identical cards want an even split, while a 16 GB card next to two
// 8 GB ones usually wants the big one filled first. Both are offered.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace crucible {

/// One device ggml registered, in ggml's own index order -- which is what
/// `main_gpu` and `tensor_split` are indexed by.
struct ComputeDevice {
    int         index = 0;
    std::string name;         ///< ggml's short name, e.g. "CUDA0"
    std::string description;  ///< the human one, e.g. "NVIDIA GeForce RTX 4070"
    std::string backend;      ///< the registry that owns it, e.g. "CUDA"

    std::uint64_t memory_total = 0;
    std::uint64_t memory_free  = 0;

    /// False for the CPU device, which is always present and is never part of
    /// a GPU split.
    bool is_gpu = false;

    /// "NVIDIA GeForce RTX 4070 (8.0 GB)"
    std::string label() const;
};

/// Every device ggml knows about. Empty until a runtime has been loaded.
std::vector<ComputeDevice> compute_devices();

/// Just the GPUs, in ggml index order, de-duplicated the way llama.cpp does.
///
/// Two backends can see the same card -- install CUDA and Vulkan and every
/// NVIDIA GPU appears twice. llama.cpp keeps the first of each and Crucible has
/// to agree with it exactly, because a split is handed over as an array indexed
/// by position in that list: disagree by one entry and every card gets the
/// share meant for its neighbor.
std::vector<ComputeDevice> gpu_devices();

/// The `tensor_split` array llama.cpp reads, from a plan indexed by ggml device
/// index.
///
/// Two translations in one, and both are load-bearing.
///
/// llama.cpp indexes tensor_split by *position among the GPUs it selected*, not
/// by ggml device index. Those agree only when the GPUs occupy the first
/// indices with nothing else before them -- true with CUDA alone on most
/// machines, and false the moment a BLAS device registers first or a second
/// backend adds cards of its own. Crucible plans in device-index space because
/// that is what `/devices` prints and what the priority order is written in, so
/// the conversion happens here, once, at the boundary.
///
/// And the array is sized to `llama_max_devices()`, because that is how many
/// floats llama.cpp copies out of the pointer it is given -- a shorter buffer
/// is read past the end.
std::vector<float> llama_tensor_split(const std::vector<ComputeDevice>& gpus,
                                      const std::vector<float>& split);

/// The `main_gpu` llama.cpp expects: the same translation, for the same reason.
/// It bounds-checks this against its own device count, so a ggml index that is
/// past the end of that list would fail the load outright.
int llama_main_gpu(const std::vector<ComputeDevice>& gpus, int main_gpu);

/// How work is divided between GPUs.
enum class GpuSplitMode {
    /// Let llama.cpp decide. One GPU, or a backend that cannot split, ends up
    /// here whatever else is configured.
    Auto,
    /// Proportional to each card's memory, so every GPU finishes its share at
    /// about the same time. The right default for cards of different sizes.
    Even,
    /// Fill the GPUs in a stated order, spilling into the next only when the
    /// previous is full. Keeps a model whole on the fastest card when it fits,
    /// and leaves the others free for something else.
    ///
    /// This one needs to know how big the model is: "fill the first card"
    /// means nothing until there is something to fill it with. See
    /// compute_tensor_split.
    Priority,
    /// Everything on one device, named by `main_gpu`.
    Single,
};

std::string_view gpu_split_mode_id(GpuSplitMode mode);
GpuSplitMode     gpu_split_mode_from_id(std::string_view id);

/// Lay a configured priority order over the devices that actually exist.
///
/// Returns `gpus` rearranged: the ones `order` names first, in that order, then
/// everything `order` did not mention, in ggml index order. An index naming a
/// card that is not in the machine is dropped rather than leaving a hole, and a
/// card that appeared since the order was written lands at the end instead of
/// vanishing -- so a config written with two GPUs plugged in still means
/// something on the day only one is.
std::vector<ComputeDevice> apply_priority_order(const std::vector<ComputeDevice>& gpus,
                                                const std::vector<int>& order);

/// What a model will want from the cards.
///
/// Three parts, because they are placed by three different rules. Weights, KV
/// cache and recurrent state follow the layers, so they are what gets divided.
/// The activation buffers do not -- every card needs its own regardless of how
/// little of the model it holds -- so they are set aside from each card's
/// capacity before anything is placed. The logits buffer is different again: it
/// is large (300 MB for a 152k vocabulary at a batch of 512) and it lives only
/// on the card that ends up with the output unit, so charging it to every card
/// would refuse models that fit.
///
/// `units` is what makes the plan exact. llama.cpp places whole layers, so a
/// target expressed only in bytes is rounded to the nearest layer boundary --
/// a 680 MB step on a 48-layer model, which is enough to overfill a card the
/// arithmetic said had room to spare. With the per-unit costs in hand the
/// split is built from a layer count instead, and the rounding disappears.
/// Leave it empty when the model's header could not be read; the split then
/// falls back to dividing proportionally.
///
/// All of it comes from the model's own header. See llm/model_shape.hpp.
struct ModelFit {
    std::uint64_t resident     = 0;  ///< weights + cache + state, divided between cards
    std::uint64_t per_card     = 0;  ///< activation buffers, needed on each card
    std::uint64_t output_extra = 0;  ///< logits, only on the card holding the output

    /// Bytes per placeable unit, in llama.cpp's order: layer 0 first, the
    /// output last. See ModelShape::units.
    std::vector<std::uint64_t> units;
};

/// A finished arrangement: what to hand llama.cpp, and what it will do.
struct GpuPlan {
    /// `tensor_split`, indexed by ggml device index. Empty means "no opinion".
    std::vector<float> split;

    /// How many units land on each device, indexed the same way. Empty when
    /// the plan was made proportionally rather than unit by unit.
    std::vector<int> units;
};

/// Room left on every card beyond what the arithmetic accounts for.
///
/// Free video memory is a moving target: a compositor redraws, a browser opens
/// a tab, and the figure a plan was made from is stale by the time the weights
/// are uploaded. This is the margin that absorbs that -- and the reason it is
/// worth having is that a load which misses fails at the very last allocation,
/// with the whole model already on the cards.
inline constexpr std::uint64_t kCardHeadroom = 256ULL * 1024 * 1024;

/// The bytes of `gpu` a model's weights may be given, after `reserve` is set
/// aside for that card's compute buffers.
///
/// Free memory rather than total: a card with a desktop compositor or another
/// model on it has less to offer than its spec sheet says, and the whole point
/// of filling in order is to know when a card is full.
std::uint64_t usable_memory(const ComputeDevice& gpu, std::uint64_t reserve = 0);

/// Turn a split mode into the `tensor_split` vector llama.cpp wants: one
/// weight per device, in ggml index order, summing to 1.
///
/// `order` lists device indices best-first and is only read in Priority mode.
/// An empty result means "no opinion", which llama.cpp reads as its default.
///
/// `fit` describes the model this split is for, and only Priority mode reads
/// it -- filling cards in order is not expressible as a proportion until you
/// know how much there is to spread. A zero `resident` means "not known", and
/// Priority then falls back to dividing by capacity, which at least never
/// hands a card more than it can hold.
std::vector<float> compute_tensor_split(GpuSplitMode mode,
                                        const std::vector<ComputeDevice>& gpus,
                                        const std::vector<int>& order,
                                        int main_gpu,
                                        const ModelFit& fit = {});

/// The same arrangement, with the layer counts it was built from.
///
/// compute_tensor_split is this without the counts. Callers that want to say
/// what the split will actually do -- "the 5060 Ti takes 21 layers of 49" --
/// want this one.
GpuPlan plan_gpu_split(GpuSplitMode mode,
                       const std::vector<ComputeDevice>& gpus,
                       const std::vector<int>& order,
                       int main_gpu,
                       const ModelFit& fit = {});

/// Which of the GPU settings the loaded devices can actually act on.
///
/// A setting the hardware cannot honor is worse than a missing one: it reads
/// as configured, it saves, and nothing happens. So each field is either empty
/// -- the setting works -- or the reason it does not, phrased for the person
/// looking at it and naming what would have to change.
///
/// Read from the devices rather than from the config, because it is whatever
/// is loaded that honors a setting or ignores it. `describe_split` lives here
/// for the same reason: the explanation belongs with the fact.
struct GpuSettingSupport {
    std::string split;      ///< dividing one model between several cards
    std::string gpu_only;   ///< keeping every layer off the processor
    std::string vram_only;  ///< refusing a model that will not fit in device memory
};

GpuSettingSupport gpu_setting_support(const std::vector<ComputeDevice>& devices);

/// A one-line explanation of what a plan will actually do, for the settings
/// screen -- "RTX 5060 Ti 21 layers, RTX 4070 13, RTX 3060 15".
///
/// Layers rather than percentages wherever the plan knows them: a percentage
/// of a model the reader cannot see the size of says very little, and the
/// layer counts are what llama.cpp is actually being told.
std::string describe_split(const std::vector<ComputeDevice>& gpus,
                           const GpuPlan& plan);

}  // namespace crucible
