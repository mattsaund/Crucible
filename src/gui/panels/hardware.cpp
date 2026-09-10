// SPDX-License-Identifier: MIT
//
// The Hardware page: what Crucible computes on, and how a model is spread over
// it when there is more than one card.
//
// The settings here are machine-wide rather than per-expert, because they are
// about the machine. They are translated into llama.cpp's own knobs at load
// time by gpu_policy.hpp -- nothing on this page writes a tensor_split itself.
//
// Every control is drawn against what the hardware actually supports, from
// gpu_setting_support(): a setting that cannot do anything here is shown
// disabled with the reason, which is more use than hiding it and leaving
// someone to wonder where it went.
#include "../app.hpp"

#include <algorithm>
#include <vector>

#include <imgui.h>

#include "crucible/runtime/devices.hpp"

#include "../theme.hpp"
#include "../widgets.hpp"

namespace crucible::gui {

namespace {

/// The priority order as a complete list of device indices.
///
/// The config stores only what the user arranged, which may be shorter than
/// the machine's list -- a card added since, or a config written on another
/// machine. Anything missing goes on the end in device order, so the list on
/// screen is always every GPU exactly once.
std::vector<int> full_order(const std::vector<ComputeDevice>& gpus,
                            const std::vector<int>&           configured) {
    std::vector<int> order;
    for (int index : configured) {
        const bool real = std::any_of(gpus.begin(), gpus.end(),
                                      [index](const ComputeDevice& gpu) {
                                          return gpu.index == index;
                                      });
        const bool already = std::find(order.begin(), order.end(), index) != order.end();
        if (real && !already) {
            order.push_back(index);
        }
    }
    for (const ComputeDevice& gpu : gpus) {
        if (std::find(order.begin(), order.end(), gpu.index) == order.end()) {
            order.push_back(gpu.index);
        }
    }
    return order;
}

const ComputeDevice* device_at(const std::vector<ComputeDevice>& gpus, int index) {
    for (const ComputeDevice& gpu : gpus) {
        if (gpu.index == index) {
            return &gpu;
        }
    }
    return nullptr;
}

}  // namespace

void App::draw_settings_hardware() {
    title("Hardware");

    const std::vector<ComputeDevice> devices = compute_devices();
    const std::vector<ComputeDevice> gpus    = gpu_devices();
    const GpuSettingSupport          support = gpu_setting_support(devices);

    section("DEVICES");
    if (devices.empty()) {
        wrapped(theme::kTextDim,
                "No compute devices, because no runtime is installed. Build one on "
                "the Runtimes page: it compiles a GPU backend for this machine.");
        if (ImGui::Button("Go to Runtimes", ImVec2(em(12.0F), 0))) {
            settings_page_ = SettingsPage::Runtimes;
        }
    }
    for (const ComputeDevice& device : devices) {
        text_colored(theme::kTextDim, "[%d] %s  %s", device.index,
                      device.label().c_str(), device.backend.c_str());
    }

    section("MULTI-GPU");
    if (!support.split.empty()) {
        wrapped(theme::kTextFaint, support.split);
    }
    ImGui::BeginDisabled(!support.split.empty());

    // The four modes, spelled the way devices.hpp spells them so the config
    // and the screen cannot disagree.
    struct Mode { GpuSplitMode mode; const char* label; const char* help; };
    static const Mode kModes[] = {
        {GpuSplitMode::Auto,     "Automatic",
         "Let llama.cpp decide. One card always ends up here."},
        {GpuSplitMode::Even,     "Even, by memory",
         "Proportional to each card's free memory, so they finish together. "
         "The right choice for cards of different sizes."},
        {GpuSplitMode::Priority, "Priority order",
         "Fill the cards in the order below, spilling into the next only when "
         "one is full. Keeps a model whole on the fastest card when it fits."},
        {GpuSplitMode::Single,   "One card only",
         "Everything on the card chosen below."},
    };
    const GpuSplitMode mode = gpu_split_mode_from_id(config_.gpu.mode);
    ImGui::SetNextItemWidth(em(16.0F));
    const char* label = "Automatic";
    for (const Mode& option : kModes) {
        if (option.mode == mode) { label = option.label; }
    }
    if (ImGui::BeginCombo("Split", label)) {
        for (const Mode& option : kModes) {
            if (ImGui::Selectable(option.label, option.mode == mode)) {
                const std::string id(gpu_split_mode_id(option.mode));
                update_config([&id](Config& config) { config.gpu.mode = id; });
            }
            ImGui::SetItemTooltip("%s", option.help);
        }
        ImGui::EndCombo();
    }

    if (mode == GpuSplitMode::Single && !gpus.empty()) {
        int main_gpu = config_.gpu.main_gpu;
        ImGui::SetNextItemWidth(em(16.0F));
        const ComputeDevice* chosen = device_at(gpus, main_gpu);
        if (ImGui::BeginCombo("Card", chosen != nullptr ? chosen->label().c_str()
                                                        : "(none)")) {
            for (const ComputeDevice& gpu : gpus) {
                if (ImGui::Selectable(gpu.label().c_str(), gpu.index == main_gpu)) {
                    const int picked = gpu.index;
                    update_config([picked](Config& config) {
                        config.gpu.main_gpu = picked;
                    });
                }
            }
            ImGui::EndCombo();
        }
    }

    // The cards, and -- only in priority mode -- the arrows that order them.
    //
    // The arrows used to be here whatever the mode was, on the reasoning that
    // the order is worth seeing because it is what priority *will* mean. That
    // reads as a control that works: you rearrange the list, nothing changes,
    // and nothing on screen says why. In every other mode the order is not
    // consulted at all, so what is left is what those modes actually use --
    // which cards there are and how big they are.
    if (gpus.size() > 1) {
        ImGui::Dummy(ImVec2(0, em(0.3F)));
        const bool ordering = mode == GpuSplitMode::Priority;
        text_colored(ordering ? theme::kText : theme::kTextFaint,
                      ordering ? "Priority order  --  filled top first" : "Cards");

        const std::vector<int> order =
            ordering ? full_order(gpus, config_.gpu.priority) : std::vector<int>();

        if (!ordering) {
            for (const ComputeDevice& gpu : gpus) {
                text_colored(theme::kTextDim, "   [%d] %s", gpu.index,
                              gpu.label().c_str());
            }
        }
        for (std::size_t row = 0; row < order.size(); ++row) {
            const ComputeDevice* gpu = device_at(gpus, order[row]);
            if (gpu == nullptr) {
                continue;
            }
            ImGui::PushID(static_cast<int>(row));

            ImGui::BeginDisabled(row == 0);
            if (ImGui::ArrowButton("up", ImGuiDir_Up)) {
                std::vector<int> moved = order;
                std::swap(moved[row], moved[row - 1]);
                update_config([&moved](Config& config) { config.gpu.priority = moved; });
            }
            ImGui::EndDisabled();
            ImGui::SameLine();

            ImGui::BeginDisabled(row + 1 >= order.size());
            if (ImGui::ArrowButton("down", ImGuiDir_Down)) {
                std::vector<int> moved = order;
                std::swap(moved[row], moved[row + 1]);
                update_config([&moved](Config& config) { config.gpu.priority = moved; });
            }
            ImGui::EndDisabled();
            ImGui::SameLine();

            text_colored(row == 0 ? theme::kFlame : theme::kTextDim, "%zu. [%d] %s",
                          row + 1, gpu->index, gpu->label().c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndDisabled();

    section("MEMORY");

    // A disabled widget does not report hover, so the reason it is disabled
    // cannot live in a tooltip -- it has to be on the page or it is invisible
    // exactly when it is needed.
    const auto setting = [](bool supported, const char* why_not, const char* help) {
        if (supported) {
            ImGui::SetItemTooltip("%s", help);
        } else {
            text_colored(theme::kTextFaint, "%s", why_not);
        }
    };

    bool gpu_only = config_.gpu.gpu_only;
    ImGui::BeginDisabled(!support.gpu_only.empty());
    if (ImGui::Checkbox("Keep every layer on the GPU", &gpu_only)) {
        update_config([gpu_only](Config& config) { config.gpu.gpu_only = gpu_only; });
    }
    ImGui::EndDisabled();
    setting(support.gpu_only.empty(), support.gpu_only.c_str(),
            "A model 90% offloaded runs at roughly the speed of one not offloaded "
            "at all, because the processor's share is two orders of magnitude "
            "slower.");

    bool vram_only = config_.gpu.vram_only;
    ImGui::BeginDisabled(!support.vram_only.empty());
    if (ImGui::Checkbox("Use dedicated VRAM only", &vram_only)) {
        update_config([vram_only](Config& config) { config.gpu.vram_only = vram_only; });
    }
    ImGui::EndDisabled();
    setting(support.vram_only.empty(), support.vram_only.c_str(),
            "Refuse a model that will not fit in dedicated video memory. Otherwise "
            "the driver spills into system RAM and the model runs about twenty "
            "times slower, with nothing on screen to say why.");
}

}  // namespace crucible::gui
