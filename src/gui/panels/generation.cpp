// SPDX-License-Identifier: MIT
//
// The Generation page: every knob llama.cpp takes, in the order they matter.
//
// These are the config's `defaults`, inherited by every expert that does not
// override them. That is the same arrangement the terminal program edits, so a
// number set here is the number the terminal shows.
//
// Loading before sampling, because getting the loading wrong costs you a model
// that will not run and getting the sampling wrong only costs you a worse
// answer. Each control writes through update_config on change, so there is no
// Apply button and nothing to lose by clicking away.
#include "../app.hpp"

#include <algorithm>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "../theme.hpp"
#include "../widgets.hpp"

namespace crucible::gui {

namespace {

/// One control width for the whole page, so the values line up in a column
/// instead of ending wherever their labels happen to.
void field() { ImGui::SetNextItemWidth(em(10.0F)); }

}  // namespace

void App::draw_settings_generation() {
    title("Generation");
    text_colored(theme::kTextFaint, "Defaults every expert inherits.");

    section("LOADING");

    int context = config_.defaults.n_ctx;
    field();
    if (ImGui::InputInt("Context size", &context, 1024, 4096)) {
        context = std::clamp(context, 512, 1 << 20);
        update_config([context](Config& config) { config.defaults.n_ctx = context; });
    }
    ImGui::SetItemTooltip("Tokens of context per expert. More context costs "
                          "memory whether or not you use it.");

    int layers = config_.defaults.n_gpu_layers;
    field();
    if (ImGui::InputInt("GPU layers", &layers, 1, 8)) {
        layers = std::clamp(layers, -1, 1024);
        update_config([layers](Config& config) {
            config.defaults.n_gpu_layers = layers;
        });
    }
    ImGui::SetItemTooltip("-1 offloads as much as fits. Ignored while "
                          "\"Keep every layer on the GPU\" is on.");

    int batch = config_.defaults.n_batch;
    field();
    if (ImGui::InputInt("Batch size", &batch, 64, 256)) {
        batch = std::clamp(batch, 1, 1 << 16);
        update_config([batch](Config& config) { config.defaults.n_batch = batch; });
    }
    ImGui::SetItemTooltip("How many tokens of your prompt are ingested at once.");

    int threads = config_.defaults.n_threads;
    field();
    if (ImGui::InputInt("Threads", &threads, 1, 4)) {
        threads = std::clamp(threads, 0, 1024);
        update_config([threads](Config& config) {
            config.defaults.n_threads = threads;
        });
    }
    ImGui::SetItemTooltip("0 picks automatically from the machine.");

    bool flash = config_.defaults.flash_attn;
    if (ImGui::Checkbox("Flash attention", &flash)) {
        update_config([flash](Config& config) { config.defaults.flash_attn = flash; });
    }
    ImGui::SetItemTooltip("Faster attention where the backend supports it.");

    // The four llama.cpp spellings, in the order of how much they divide.
    static const char* const kSplitModes[] = {"none", "layer", "row", "tensor"};
    const std::string current = config_.defaults.split_mode;
    field();
    if (ImGui::BeginCombo("Split granularity", current.c_str())) {
        for (const char* mode : kSplitModes) {
            if (ImGui::Selectable(mode, current == mode)) {
                update_config([mode](Config& config) {
                    config.defaults.split_mode = mode;
                });
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("How a model is divided when it is spread over more "
                          "than one card. \"layer\" is right almost always.");

    section("SAMPLING");

    float temperature = config_.defaults.temperature;
    field();
    if (ImGui::SliderFloat("Temperature", &temperature, 0.0F, 2.0F, "%.2f")) {
        update_config([temperature](Config& config) {
            config.defaults.temperature = temperature;
        });
    }
    ImGui::SetItemTooltip("0 is greedy and deterministic.");

    float top_p = config_.defaults.top_p;
    field();
    if (ImGui::SliderFloat("Top-p", &top_p, 0.0F, 1.0F, "%.2f")) {
        update_config([top_p](Config& config) { config.defaults.top_p = top_p; });
    }
    ImGui::SetItemTooltip("Nucleus sampling cutoff.");

    int top_k = config_.defaults.top_k;
    field();
    if (ImGui::SliderInt("Top-k", &top_k, 0, 200)) {
        update_config([top_k](Config& config) { config.defaults.top_k = top_k; });
    }
    ImGui::SetItemTooltip("Candidates kept before sampling. 0 disables it.");

    float min_p = config_.defaults.min_p;
    field();
    if (ImGui::SliderFloat("Min-p", &min_p, 0.0F, 1.0F, "%.3f")) {
        update_config([min_p](Config& config) { config.defaults.min_p = min_p; });
    }
    ImGui::SetItemTooltip("Minimum probability relative to the best candidate.");

    float repeat = config_.defaults.repeat_penalty;
    field();
    if (ImGui::SliderFloat("Repeat penalty", &repeat, 1.0F, 2.0F, "%.2f")) {
        update_config([repeat](Config& config) {
            config.defaults.repeat_penalty = repeat;
        });
    }
    ImGui::SetItemTooltip("1.0 disables it.");

    int max_tokens = config_.defaults.max_tokens;
    field();
    if (ImGui::InputInt("Max tokens", &max_tokens, 128, 512)) {
        max_tokens = std::clamp(max_tokens, -1, 1 << 20);
        update_config([max_tokens](Config& config) {
            config.defaults.max_tokens = max_tokens;
        });
    }
    ImGui::SetItemTooltip("Hard cap on a single reply. -1 runs until the model stops.");

    section("PROMPTING");

    std::string prompt = config_.system_prompt;
    text_colored(theme::kTextFaint, "Sent ahead of every prompt.");
    if (ImGui::InputTextMultiline("##system-prompt", &prompt,
                                  ImVec2(-FLT_MIN, em(6.0F)))) {
        update_config([&prompt](Config& config) { config.system_prompt = prompt; });
    }

    static const char* const kEfforts[] = {"low", "medium", "high"};
    const std::string effort = config_.reasoning_effort;
    ImGui::SetNextItemWidth(em(10.0F));
    if (ImGui::BeginCombo("Reasoning effort", effort.c_str())) {
        for (const char* level : kEfforts) {
            if (ImGui::Selectable(level, effort == level)) {
                update_config([level](Config& config) {
                    config.reasoning_effort = level;
                });
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("How hard a thinking model works before it answers. "
                          "Ignored by models that do not think.");
}

}  // namespace crucible::gui
