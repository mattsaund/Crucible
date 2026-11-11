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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crucible {

/// One device ggml registered, in ggml's own index order -- which is what
/// `main_gpu` and `tensor_split` are indexed by.
struct ComputeDevice {
    int         index = 0;
    std::string name;         ///< ggml's short name, e.g. "CUDA0"
    std::string description;  ///< the human one, e.g. "NVIDIA GeForce RTX 4070"
    std::string backend;      ///< the registry that owns it, e.g. "CUDA"
    std::string device_id;    ///< the driver's id for the card, its PCI address where it has one

    std::uint64_t memory_total = 0;
    std::uint64_t memory_free  = 0;

    /// How fast weights reach it from the processor, in GB a second: its PCIe
    /// link, as the slot it sits in allows. Zero when unknown -- a platform
    /// that does not say, or a GPU that shares the processor's memory.
    ///
    /// The number that decides how long a load takes. A card in a slot wired
    /// for one lane takes a gigabyte a second, a sixteenth of what the slot
    /// beside it does, so which card a model's layers go to is the difference
    /// between a load of five seconds and one of twenty.
    double link_gbps = 0.0;

    /// It draws the desktop. A model leaves it room (see kDisplayRoom): what
    /// the desktop holds there moves while a model is loaded and used.
    bool display = false;

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

/// The GPUs of `devices` as llama.cpp keeps them: a card is the same card
/// only when both report a device id and the ids match. Two cards of one
/// model share a name, never an id.
std::vector<ComputeDevice> distinct_gpus(std::vector<ComputeDevice> devices);

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
/// How many of `units` layers llama.cpp gives each device for `split`.
///
/// Its own rule, replayed: the split is made cumulative and normalized, and
/// layer `i` goes to the first device whose share exceeds `i / units`. Indexed
/// the way `split` is. Anything that wants to know what a split will actually
/// put on a card has to ask this rather than multiply by the share, because
/// layers are not the same size -- the last one carries the output weights,
/// and on a card filled to the brim the difference is the whole margin.
std::vector<int> llama_layer_assignment(const std::vector<float>& split, std::size_t units);

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
/// everything `order` did not mention: the fastest link first, then the most
/// memory, then ggml index order. An index naming a card that is not in the
/// machine is dropped rather than leaving a hole, and a card that appeared
/// since the order was written lands at the end instead of vanishing -- so a
/// config written with two GPUs plugged in still means something on the day
/// only one is.
///
/// The fastest link first because that is where a model loads fastest, and
/// with no order written, nothing else says which card is best. On a machine
/// with a card on sixteen lanes and two on one, llama.cpp's own split put a
/// 14 GB model across all three, 9 of its gigabytes on the one-lane cards,
/// and its load took twice what filling the fast card first does. That beat
/// the one-lane 16 GB card holding it whole on both counts, measured: 5.9 s
/// against 14.8 to load, 89 tokens a second against 80. Between links alike,
/// the larger card first, since it is the one likeliest to hold a model
/// without splitting it.
std::vector<ComputeDevice> apply_priority_order(const std::vector<ComputeDevice>& gpus,
                                                const std::vector<int>& order);

/// Whether the cards' links are all known and one is at least half again as
/// fast as another: a machine where which card a model goes to decides how
/// long it takes to load. See GpuSplitMode::Auto in gpu_policy.hpp.
bool links_differ(const std::vector<ComputeDevice>& gpus);

/// A card's PCIe link, from what the system says of it. Linux says it in
/// sysfs and Windows in each device's properties. Only the asking differs,
/// so the rules are here, where a test on any system can check them.
namespace pci {

/// Where a device sits: the PCI domain, zero on all but the largest servers,
/// the bus, and the device and function on it.
struct Address {
    std::uint32_t domain   = 0;
    std::uint32_t bus      = 0;
    std::uint32_t device   = 0;
    std::uint32_t function = 0;

    bool operator==(const Address&) const = default;
};

/// `id` read as the bus id ggml gives a card for its device id, which CUDA
/// and Vulkan both write "0000:01:00.0": domain, bus, device and function,
/// in hex. Nothing for anything else, a CUDA card split into virtual devices
/// ("0000:01:00.0-v1") included, since no slot goes by that name.
std::optional<Address> parse_bus_id(std::string_view id);

/// Where Windows puts a PCI function by its bus number and its address
/// (DEVPKEY_Device_BusNumber and DEVPKEY_Device_Address), with the device in
/// the address's high word and the function in its low one. Windows does not
/// say the domain, so it is zero. Nothing for an address the bus driver did
/// not give, which is all ones, or one no PCI function can have.
std::optional<Address> address_of(std::uint32_t bus, std::uint32_t address);

/// A card's slot as the system says it. Rates are GT/s and widths are
/// lanes, zero for whatever it does not say.
struct Slot {
    double card_rate  = 0.0;  ///< the fastest the card can run
    double port_rate  = 0.0;  ///< the fastest the port it is plugged into can
    double lanes      = 0.0;  ///< the lanes the card and the slot agreed on
    double card_lanes = 0.0;  ///< the card's own most, for when `lanes` is not said
    double port_lanes = 0.0;  ///< the port's most
};

/// GB a second over `slot`, the figure ComputeDevice::link_gbps holds: the
/// slower of the two fastest rates, over the lanes agreed. Zero when the
/// rate or the lanes are not known.
///
/// The fastest rates, and not the one the link runs at this moment, which is
/// no use: an idle card drops to the slowest one and only speeds up under
/// load.
double link_gbps(const Slot& slot);

/// The GT/s of a link speed as Windows gives it (DEVPKEY_PciDevice_MaxLinkSpeed
/// and the like), which is the PCIe registers' own code: 1 for 2.5 GT/s, 2 for
/// 5, 3 for 8, 4 for 16, 5 for 32 and 6 for 64. Zero for any other.
double rate_of_code(std::uint32_t code);

/// One PCI function as Windows lists it: a card, or the port of a root
/// complex or a switch that a card is plugged into.
struct Function {
    Address       address;
    std::string   instance;       ///< its device instance id
    std::string   parent;         ///< the instance id of what it is plugged into
    std::uint32_t max_speed = 0;  ///< the fastest it can run, a code for rate_of_code
    std::uint32_t lanes     = 0;  ///< the lanes agreed on
    std::uint32_t max_lanes = 0;  ///< its own most
};

/// What the system says of the slot a card is in.
struct Link {
    double gbps    = 0.0;    ///< see ComputeDevice::link_gbps
    bool   display = false;  ///< see ComputeDevice::display
};

/// The link of the card at `card` among `functions`, through the port it is
/// plugged into, which is its parent, and whether it draws the desktop, as
/// the one whose instance id is `desktop` does. Unknown for a card not listed,
/// or listed twice, which is two domains Windows cannot tell apart.
Link card_link(const std::vector<Function>& functions, const Address& card, std::string_view desktop);

/// What this system says of the slot of the card whose device id ggml gives
/// as `bus_id`, which Linux says in sysfs and Windows in the devices'
/// properties. Asked once a card, since a slot does not change while
/// Crucible runs. Unknown on a Mac, whose GPU shares the processor's memory.
Link link_of(const char* bus_id);

}  // namespace pci

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

/// What a dry run says a load puts on one card, in llama.cpp's own figures:
/// a load with nothing allocated, which places every tensor, sizes the cache
/// and reserves the compute graph exactly as the real one will.
struct CardNeed {
    std::uint64_t weights = 0;  ///< the model's tensors
    std::uint64_t cache   = 0;  ///< KV cache and recurrent state
    std::uint64_t compute = 0;  ///< the buffers the graph computes in

    std::uint64_t total() const { return weights + cache + compute; }
};

/// `fit`, corrected by a dry run of the plan it made.
///
/// The header gives the weights and the cache to the byte and nothing about
/// the compute buffers, so a plan from it sets 512 MiB aside on every card
/// for them, and the logits besides on the card with the output: 805 MiB
/// there for flint. A dry run of flint at 8K over two devices put them at 194
/// MiB on the card without the output and 393 on the one with it.
/// Calibrated, `per_card` is the largest compute buffer measured on a card
/// without the output plus `runtime`, what the backend takes beyond the
/// buffers llama.cpp reports; `output_extra` is what the card with the
/// output measured over that; and the units are scaled by the most the
/// header's weights and cache missed the measured ones by on any card.
///
/// `counts` is how many units the measured plan put on each card and `need`
/// what the dry run found there, both indexed by ggml device index. When the
/// two do not describe the whole of `fit`, `fit` comes back unchanged.
ModelFit calibrated_fit(const ModelFit& fit, const std::vector<int>& counts,
                        const std::vector<CardNeed>& need, std::uint64_t runtime);

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

/// What a plan leaves free on the card drawing the desktop, beyond
/// kCardHeadroom, when the model fits without it. A compositor and a browser
/// there took 2.4 GB and move by most of a gigabyte as windows and videos
/// open; a card filled to its last 900 MB left them nothing to move into.
inline constexpr std::uint64_t kDisplayRoom = 768ULL * 1024 * 1024;

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
