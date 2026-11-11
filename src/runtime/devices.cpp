// SPDX-License-Identifier: MIT
//
// Device enumeration and the GPU split policy. See devices.hpp for the why.
#include "crucible/runtime/devices.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <system_error>
#include <utility>

#include <ggml-backend.h>
#include <llama.h>

#include "crucible/runtime/backend.hpp"
#include "crucible/runtime/registry.hpp"
#include "crucible/util/format.hpp"

// After everything else, so none of the macros SetupAPI and its headers
// define reaches the headers above.
#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <setupapi.h>
// Last, and with INITGUID set, so this file defines the property keys it
// reads: without it, devpkey.h and pciprop.h only declare them, and nothing
// else defines them for the linker.
#  include <initguid.h>
#  include <devpkey.h>
#  include <pciprop.h>
#endif

namespace crucible {

namespace pci {
namespace {

/// The hex number at the front of `text`, taken off it. Nothing when `text`
/// does not start with one.
std::optional<std::uint32_t> take_hex(std::string_view& text) {
    std::uint32_t value = 0;
    const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (parsed.ec != std::errc() || parsed.ptr == text.data()) {
        return std::nullopt;
    }
    text.remove_prefix(static_cast<std::size_t>(parsed.ptr - text.data()));
    return value;
}

}  // namespace

std::optional<Address> parse_bus_id(std::string_view id) {
    // Four hex numbers, each but the last followed by its separator, and
    // each no larger than PCI allows.
    Address              address;
    std::uint32_t* const fields[]     = {&address.domain, &address.bus, &address.device, &address.function};
    const char           separators[] = {':', ':', '.'};
    const std::uint32_t  most[]       = {0xFFFFFFFFU, 0xFFU, 0x1FU, 0x7U};
    for (std::size_t i = 0; i < 4; ++i) {
        const std::optional<std::uint32_t> value = take_hex(id);
        if (!value || *value > most[i]) {
            return std::nullopt;
        }
        *fields[i] = *value;
        if (i < 3) {
            if (id.empty() || id.front() != separators[i]) {
                return std::nullopt;
            }
            id.remove_prefix(1);
        }
    }
    if (!id.empty()) {
        return std::nullopt;
    }
    return address;
}

std::optional<Address> address_of(std::uint32_t bus, std::uint32_t address) {
    Address at;
    at.bus      = bus;
    at.device   = address >> 16;
    at.function = address & 0xFFFFU;
    if (at.bus > 0xFFU || at.device > 0x1FU || at.function > 0x7U) {
        return std::nullopt;
    }
    return at;
}

double link_gbps(const Slot& slot) {
    double rate = slot.card_rate;
    if (slot.port_rate > 0.0) {
        rate = rate > 0.0 ? std::min(rate, slot.port_rate) : slot.port_rate;
    }
    double lanes = slot.lanes > 0.0 ? slot.lanes : slot.card_lanes;
    if (slot.port_lanes > 0.0 && lanes > slot.port_lanes) {
        lanes = slot.port_lanes;
    }
    // A lane carries four fifths of its rate up to 5 GT/s (8b/10b) and
    // 128/130 of it from 8 GT/s on (128b/130b), in bits: a byte is eight.
    const double lane = rate < 8.0 ? rate * 0.8 / 8.0 : rate * 128.0 / 130.0 / 8.0;
    return lane * lanes;
}

double rate_of_code(std::uint32_t code) {
    // Windows hands the PCIe registers' own code on as it is. pciprop.h names
    // only 1 and 2, written when 5 GT/s was the fastest there was, and a
    // third generation card reads 3.
    switch (code) {
        case 1:  return 2.5;
        case 2:  return 5.0;
        case 3:  return 8.0;
        case 4:  return 16.0;
        case 5:  return 32.0;
        case 6:  return 64.0;
        default: return 0.0;
    }
}

Link card_link(const std::vector<Function>& functions, const Address& card, std::string_view desktop) {
    // By bus, device and function alone, since Windows does not say the
    // domain. The same three in two domains are two cards, and which is
    // which cannot be told.
    const Function* found = nullptr;
    for (const Function& function : functions) {
        const Address& at = function.address;
        if (at.bus == card.bus && at.device == card.device && at.function == card.function) {
            if (found != nullptr) {
                return {};
            }
            found = &function;
        }
    }
    if (found == nullptr) {
        return {};
    }

    // Instance ids name the same device in any case.
    const auto same = [](std::string_view a, std::string_view b) {
        return !a.empty() && format::to_lower(a) == format::to_lower(b);
    };
    Slot slot;
    slot.card_rate  = rate_of_code(found->max_speed);
    slot.lanes      = static_cast<double>(found->lanes);
    slot.card_lanes = static_cast<double>(found->max_lanes);
    // The port is what the card hangs off: a root port of the processor or
    // the chipset, or a switch's. A card on the root complex itself, as a
    // processor's own graphics is, has none listed, and its own figures stand.
    for (const Function& port : functions) {
        if (same(port.instance, found->parent)) {
            slot.port_rate  = rate_of_code(port.max_speed);
            slot.port_lanes = static_cast<double>(port.max_lanes);
            break;
        }
    }
    Link link;
    link.gbps    = link_gbps(slot);
    link.display = same(found->instance, desktop);
    return link;
}

}  // namespace pci

namespace {

#if defined(__linux__)
/// The first number in a sysfs file: "8.0 GT/s PCIe" is 8, "16" is 16. Zero
/// when the file is not there or says "Unknown".
double sysfs_number(const std::filesystem::path& file) {
    std::ifstream in(file);
    double value = 0.0;
    return in >> value ? value : 0.0;
}

/// Linux says, under /sys/bus/pci/devices/<bus id>: the card's own fastest
/// rate, the lanes it agreed with the slot, and, one folder up, what the port
/// it is plugged into allows. See pci::link_gbps.
pci::Link read_link(const std::string& bus_id) {
    pci::Link link;
    std::error_code ec;
    const std::filesystem::path card =
        std::filesystem::canonical(std::filesystem::path("/sys/bus/pci/devices") / bus_id, ec);
    if (ec) {
        return link;
    }
    link.display = sysfs_number(card / "boot_vga") == 1.0;

    pci::Slot slot;
    slot.card_rate  = sysfs_number(card / "max_link_speed");    // GT/s
    slot.port_rate  = sysfs_number(card.parent_path() / "max_link_speed");
    slot.lanes      = sysfs_number(card / "current_link_width");
    slot.card_lanes = sysfs_number(card / "max_link_width");
    slot.port_lanes = sysfs_number(card.parent_path() / "max_link_width");
    link.gbps = pci::link_gbps(slot);
    return link;
}
#elif defined(_WIN32)
/// `wide` in UTF-8.
std::string utf8(const wchar_t* wide) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), bytes, nullptr, nullptr);
    out.pop_back();   // the terminator WideCharToMultiByte counted
    return out;
}

/// A device's string property in UTF-8, or empty when it has none. Sized
/// for the one kind asked for, an instance id, which is 200 characters at
/// most (MAX_DEVICE_ID_LEN).
std::string string_property(HDEVINFO set, SP_DEVINFO_DATA& info, const DEVPROPKEY& key) {
    wchar_t     text[256] = {};
    DEVPROPTYPE type      = DEVPROP_TYPE_EMPTY;
    if (SetupDiGetDevicePropertyW(set, &info, &key, &type, reinterpret_cast<PBYTE>(text),
                                  static_cast<DWORD>(sizeof(text) - sizeof(wchar_t)), nullptr, 0) == FALSE
        || type != DEVPROP_TYPE_STRING) {
        return {};
    }
    return utf8(text);
}

/// A device's whole-number property, or nothing when it has none, since a
/// bus number of zero is a real one.
std::optional<std::uint32_t> number_property(HDEVINFO set, SP_DEVINFO_DATA& info, const DEVPROPKEY& key) {
    ULONG       value = 0;
    DEVPROPTYPE type  = DEVPROP_TYPE_EMPTY;
    if (SetupDiGetDevicePropertyW(set, &info, &key, &type, reinterpret_cast<PBYTE>(&value),
                                  static_cast<DWORD>(sizeof(value)), nullptr, 0) == FALSE
        || (type != DEVPROP_TYPE_UINT32 && type != DEVPROP_TYPE_INT32)) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

/// Every PCI function Windows lists, with what pci::card_link reads of each.
/// Every one, and not the display adapters alone: what a card is plugged
/// into, a root port or a switch's, is one too, and says what the slot allows.
std::vector<pci::Function> pci_functions() {
    std::vector<pci::Function> functions;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"PCI", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        return functions;
    }
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info) != FALSE; ++i) {
        const std::optional<std::uint32_t> bus     = number_property(set, info, DEVPKEY_Device_BusNumber);
        const std::optional<std::uint32_t> address = number_property(set, info, DEVPKEY_Device_Address);
        if (!bus || !address) {
            continue;
        }
        const std::optional<pci::Address> at = pci::address_of(*bus, *address);
        wchar_t instance[256] = {};
        if (!at || SetupDiGetDeviceInstanceIdW(set, &info, instance, static_cast<DWORD>(std::size(instance)),
                                               nullptr) == FALSE) {
            continue;
        }
        pci::Function function;
        function.address   = *at;
        function.instance  = utf8(instance);
        function.parent    = string_property(set, info, DEVPKEY_Device_Parent);
        // The fastest the card can run, and not the rate it runs at now
        // (DEVPKEY_PciDevice_CurrentLinkSpeed), which drops while it idles.
        function.max_speed = number_property(set, info, DEVPKEY_PciDevice_MaxLinkSpeed).value_or(0);
        function.lanes     = number_property(set, info, DEVPKEY_PciDevice_CurrentLinkWidth).value_or(0);
        function.max_lanes = number_property(set, info, DEVPKEY_PciDevice_MaxLinkWidth).value_or(0);
        functions.push_back(std::move(function));
    }
    SetupDiDestroyDeviceInfoList(set);
    return functions;
}

/// The instance id of the display adapter the primary monitor is on, the
/// card the desktop is drawn on, or empty when that cannot be told.
///
/// Through the monitor: GDI says which display is the primary and gives its
/// monitor's device interface, and in SetupAPI a monitor is a child of the
/// adapter it is plugged into. GDI's own words for the adapter are its
/// model, the same for two cards of a kind.
std::string desktop_adapter() {
    DISPLAY_DEVICEW display{};
    display.cb = sizeof(display);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &display, 0) != FALSE; ++i) {
        if ((display.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) == 0) {
            continue;
        }
        DISPLAY_DEVICEW monitor{};
        monitor.cb = sizeof(monitor);
        if (EnumDisplayDevicesW(display.DeviceName, 0, &monitor, EDD_GET_DEVICE_INTERFACE_NAME) == FALSE) {
            return {};
        }
        HDEVINFO set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
        if (set == INVALID_HANDLE_VALUE) {
            return {};
        }
        // Opening the interface adds the monitor itself to the empty set.
        std::string              adapter;
        SP_DEVICE_INTERFACE_DATA opened{};
        opened.cbSize = sizeof(opened);
        SP_DEVINFO_DATA info{};
        info.cbSize = sizeof(info);
        if (SetupDiOpenDeviceInterfaceW(set, monitor.DeviceID, 0, &opened) != FALSE
            && SetupDiEnumDeviceInfo(set, 0, &info) != FALSE) {
            adapter = string_property(set, info, DEVPKEY_Device_Parent);
        }
        SetupDiDestroyDeviceInfoList(set);
        return adapter;
    }
    return {};
}

/// Windows says it in each device's properties, through SetupAPI: the
/// card's own fastest rate and the lanes it agreed with the slot, and, from
/// its parent, what the port it is plugged into allows. See pci::card_link.
pci::Link read_link(const std::string& bus_id) {
    const std::optional<pci::Address> card = pci::parse_bus_id(bus_id);
    if (!card) {
        return {};
    }
    // Asked once for all the cards, since what is plugged in where, and the
    // card the desktop is on, are the same for each.
    static const std::vector<pci::Function> functions = pci_functions();
    static const std::string                desktop   = desktop_adapter();
    return pci::card_link(functions, *card, desktop);
}
#endif

}  // namespace

pci::Link pci::link_of(const char* bus_id) {
    if (bus_id == nullptr || *bus_id == '\0') {
        return {};
    }
#if defined(__linux__) || defined(_WIN32)
    // read_link once a card, since devices are listed many times a load.
    static std::mutex                  mutex;
    static std::map<std::string, Link> known;
    const std::lock_guard<std::mutex>  lock(mutex);
    const auto found = known.find(bus_id);
    if (found != known.end()) {
        return found->second;
    }
    return known.emplace(bus_id, read_link(bus_id)).first->second;
#else
    // A Mac's GPU shares the processor's memory: there is no link to read,
    // so ggml's order stands.
    return {};
#endif
}

std::vector<int> llama_layer_assignment(const std::vector<float>& split, std::size_t units) {
    std::vector<int> counts(split.size(), 0);
    float running = 0.0F;
    std::vector<float> cumulative(split.size(), 0.0F);
    for (std::size_t i = 0; i < split.size(); ++i) {
        running      += split[i];
        cumulative[i] = running;
    }
    if (running <= 0.0F || units == 0) {
        return counts;
    }
    for (float& value : cumulative) {
        value /= running;
    }
    for (std::size_t unit = 0; unit < units; ++unit) {
        const auto share = static_cast<float>(unit) / static_cast<float>(units);
        const auto found = std::upper_bound(cumulative.begin(), cumulative.end(), share);
        const auto device = static_cast<std::size_t>(std::distance(cumulative.begin(), found));
        if (device < counts.size()) {
            ++counts[device];
        }
    }
    return counts;
}

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
    // ggml's list as it stands, copied under the lock a runtime's load takes;
    // what each device is asked below, its memory included, is asked of the
    // copy with nothing held. See RuntimeRegistry::devices.
    const std::vector<ggml_backend_dev_t> listed = RuntimeRegistry::devices();
    devices.reserve(listed.size());

    for (std::size_t i = 0; i < listed.size(); ++i) {
        ggml_backend_dev_t handle = listed[i];
        if (handle == nullptr) {
            continue;
        }

        // One question for everything, its memory included: asking for the
        // memory on its own as well would ask the driver twice.
        ggml_backend_dev_props props{};
        ggml_backend_dev_get_props(handle, &props);

        ComputeDevice device;
        device.index = static_cast<int>(i);
        if (props.name != nullptr) {
            device.name = props.name;
        }
        if (props.description != nullptr) {
            device.description = props.description;
        }
        if (props.device_id != nullptr) {
            device.device_id = props.device_id;
        }
        if (ggml_backend_reg_t reg = ggml_backend_dev_backend_reg(handle); reg != nullptr) {
            if (const char* reg_name = ggml_backend_reg_name(reg); reg_name != nullptr) {
                device.backend = reg_name;
            }
        }
        device.memory_free  = props.memory_free;
        device.memory_total = props.memory_total;
        device.is_gpu       = props.type == GGML_BACKEND_DEVICE_TYPE_GPU;
        if (device.is_gpu) {
            const pci::Link link = pci::link_of(props.device_id);
            device.link_gbps     = link.gbps;
            device.display       = link.display;
        }
        devices.push_back(std::move(device));
    }
    return devices;
}

std::vector<ComputeDevice> gpu_devices() { return distinct_gpus(compute_devices()); }

std::vector<ComputeDevice> distinct_gpus(std::vector<ComputeDevice> devices) {
    std::vector<ComputeDevice> gpus;
    for (ComputeDevice& device : devices) {
        if (!device.is_gpu) {
            continue;
        }
        // Install CUDA and Vulkan together and every NVIDIA card is reported
        // twice, under one device id; llama.cpp keeps the first of each, and a
        // split planned against a list it does not share would give every card
        // the share meant for its neighbor. By the id, as llama.cpp does, and
        // never by the name: two cards of one model have the same name, and
        // the second was left out of every split.
        const bool seen = !device.device_id.empty()
                       && std::any_of(gpus.begin(), gpus.end(), [&device](const ComputeDevice& other) {
                              return other.device_id == device.device_id;
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

    const std::size_t named = arranged.size();
    for (const ComputeDevice& gpu : gpus) {
        if (!already_taken(gpu.index)) {
            arranged.push_back(gpu);
        }
    }
    // Between cards whose links are alike -- or unknown, which is zero -- the
    // larger first: the one likeliest to hold a model whole, which then is
    // not split at all. Stable, so cards alike in both keep ggml's order.
    std::stable_sort(arranged.begin() + static_cast<std::ptrdiff_t>(named), arranged.end(),
                     [](const ComputeDevice& a, const ComputeDevice& b) {
                         if (a.link_gbps != b.link_gbps) {
                             return a.link_gbps > b.link_gbps;
                         }
                         return a.memory_total > b.memory_total;
                     });
    return arranged;
}

bool links_differ(const std::vector<ComputeDevice>& gpus) {
    if (gpus.size() < 2) {
        return false;
    }
    double fastest = 0.0;
    double slowest = 0.0;
    for (const ComputeDevice& gpu : gpus) {
        if (gpu.link_gbps <= 0.0) {
            return false;   // one unknown, and nothing can be said of the rest
        }
        fastest = std::max(fastest, gpu.link_gbps);
        slowest = slowest == 0.0 ? gpu.link_gbps : std::min(slowest, gpu.link_gbps);
    }
    return fastest >= slowest * 1.5;
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
/// card that is over its capacity, and only when no card that had room ends up
/// without it. It stops when nothing is over, or when no move helps -- which
/// means the model does not fit, and saying so is the VRAM check's job, not
/// this one's.
///
/// A move is judged by what every card holds after it, not by the unit it
/// seems to move. Units are dealt out in index order, so one less for a card
/// and one more for another shifts every boundary between the two: moving
/// down, the unit that changes hands is the first of the card's range, and
/// each card in between trades a unit for its neighbor's. Taking the last unit
/// as the one that moves put a 0.7 GB layer on a full card in place of the
/// 0.25 GB output it thought it was moving, and refused a 33 GB model that
/// three cards held.
void rebalance(std::vector<int>& counts,
               const std::vector<std::uint64_t>& units,
               const std::vector<std::uint64_t>& capacity) {
    const auto over_of = [&](const std::vector<std::uint64_t>& used, std::size_t device) {
        return used[device] > capacity[device] ? used[device] - capacity[device] : 0;
    };

    for (std::size_t guard = 0; guard <= units.size(); ++guard) {
        const std::vector<std::uint64_t> used = usage_of(counts, units);
        std::uint64_t over = 0;
        for (std::size_t device = 0; device < counts.size(); ++device) {
            over += over_of(used, device);
        }
        if (over == 0) {
            return;  // everything fits
        }

        std::vector<int> best;
        std::uint64_t    best_over = over;
        for (std::size_t from = 0; from < counts.size(); ++from) {
            if (counts[from] == 0 || over_of(used, from) == 0) {
                continue;
            }
            for (std::size_t into = 0; into < counts.size(); ++into) {
                if (into == from) {
                    continue;
                }
                std::vector<int> moved = counts;
                --moved[from];
                ++moved[into];
                const std::vector<std::uint64_t> after = usage_of(moved, units);
                std::uint64_t after_over = 0;
                bool          crowds     = false;
                for (std::size_t device = 0; device < counts.size(); ++device) {
                    after_over += over_of(after, device);
                    crowds = crowds || over_of(after, device) > over_of(used, device);
                }
                if (!crowds && after_over < best_over) {
                    best      = std::move(moved);
                    best_over = after_over;
                }
            }
        }
        if (best.empty()) {
            return;  // no move helps; the model is too large for the machine
        }
        counts = std::move(best);
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

    // The desktop's card keeps its room when the model fits without it, and
    // gives it up when it does not: a model loaded beside a desktop that is
    // a little short of room is better than one that spills or is refused.
    // Priority only: even mode divides by what each card holds, room and all.
    bool display_room = mode == GpuSplitMode::Priority
                     && std::any_of(gpus.begin(), gpus.end(), [](const ComputeDevice& gpu) { return gpu.display; });

    for (int pass = 0; pass < 2; ++pass) {
        targets.assign(width, 0);
        capacity.assign(width, 0);
        const auto reserve_for = [&](const ComputeDevice& gpu) {
            const auto index = static_cast<std::size_t>(gpu.index);
            return fit.per_card + kCardHeadroom +
                   (index == output_device ? fit.output_extra : 0) +
                   (display_room && gpu.display ? kDisplayRoom : 0);
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
            if (display_room && fit.resident > 0 && remaining > 0) {
                // Planned again from the start without the desktop's room,
                // which may be all the model was short of.
                display_room  = false;
                output_device = width;
                pass          = -1;
                continue;
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

ModelFit calibrated_fit(const ModelFit& fit, const std::vector<int>& counts,
                        const std::vector<CardNeed>& need, std::uint64_t runtime) {
    // What the header said each card would hold of the layers, and which card
    // ends up with the output: the last one in index order that holds any.
    std::vector<std::uint64_t> said(counts.size(), 0);
    std::size_t unit   = 0;
    std::size_t output = counts.size();
    for (std::size_t device = 0; device < counts.size(); ++device) {
        for (int i = 0; i < counts[device] && unit < fit.units.size(); ++i, ++unit) {
            said[device] += fit.units[unit];
        }
        if (counts[device] > 0) {
            output = device;
        }
    }
    if (fit.units.empty() || unit < fit.units.size() || output >= need.size()) {
        return fit;
    }

    // The worst the header did on any card, as a factor on every unit: right
    // to the byte on the card that set it and conservative on the rest.
    double        scale   = 0.0;
    std::uint64_t compute = 0;     // the largest buffer on a card without the output
    bool          others  = false;
    for (std::size_t device = 0; device < counts.size(); ++device) {
        if (counts[device] == 0) {
            continue;
        }
        if (device >= need.size()) {
            return fit;   // a card the dry run said nothing about
        }
        if (said[device] > 0) {
            scale = std::max(scale, static_cast<double>(need[device].weights + need[device].cache) /
                                        static_cast<double>(said[device]));
        }
        if (device != output) {
            compute = std::max(compute, need[device].compute);
            others  = true;
        }
    }
    if (scale <= 0.0) {
        return fit;
    }
    const std::uint64_t at_output = need[output].compute;
    if (!others) {
        // The whole model on one card: its buffer less the logits the header
        // puts there is the best word on what a card without them needs.
        compute = at_output > fit.output_extra ? at_output - fit.output_extra : 0;
    }

    ModelFit out;
    out.units.reserve(fit.units.size());
    for (const std::uint64_t bytes : fit.units) {
        out.units.push_back(static_cast<std::uint64_t>(std::ceil(static_cast<double>(bytes) * scale)));
        out.resident += out.units.back();
    }
    out.per_card     = compute + runtime;
    out.output_extra = at_output > compute ? at_output - compute : 0;
    return out;
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
            "no GPU runtime is loaded: install one under Runtimes";
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
