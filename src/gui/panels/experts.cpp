// SPDX-License-Identifier: MIT
//
// The expert roster as a settings page: what each seat is for, which model is
// behind it, and the buttons that add or eject one.
//
// This is the page, not the sidebar section of the same name. The sidebar says
// which experts exist and what they are doing; this is where they are changed.
#include "../app.hpp"

#include <imgui.h>

#include "../theme.hpp"
#include "../widgets.hpp"

namespace crucible::gui {

void App::draw_expert_list() {
    title("Experts");
    wrapped(theme::kTextDim,
            "The delegator routes each prompt to one of these. Add your own with a "
            "name and a description of what it handles; everything else is worked "
            "out for you.");
    ImGui::Dummy(ImVec2(0, em(0.5F)));

    if (ImGui::Button("+ New expert")) {
        expert_modal_open_ = true;
        new_expert_name_.clear();
        new_expert_blurb_.clear();
        expert_error_.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan models")) {
        refresh_models();
        say("found " + std::to_string(models_.size()) + " GGUF files");
    }
    ImGui::Dummy(ImVec2(0, em(0.5F)));

    if (config_.roster.experts().empty()) {
        wrapped(theme::kTextDim,
                "The expert list is empty. Nothing can answer until there is an expert "
                "on it.");
    }

    std::optional<ExpertId> eject;
    for (const Expert& expert : config_.roster.experts()) {
        ImGui::PushID(expert.id.c_str());
        ImGui::Separator();

        ImGui::PushFont(theme::bold());
        text_colored(theme::kText, "%s", expert.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        text_colored(theme::kTextFaint, "[%s]", expert.tag.c_str());
        if (config_.routing.default_expert == expert.id) {
            ImGui::SameLine();
            text_colored(theme::kFlame, "default");
        }
        wrapped(theme::kTextDim, expert.blurb);

        // The model assignment. A combo rather than a text box: the models
        // directory is the list of valid answers, and typing a file name is how
        // you get a seat that points at nothing.
        const ModelParams& params = config_.expert(expert.id);
        ImGui::SetNextItemWidth(em(20.0F));
        const bool open = ImGui::BeginCombo("##model", model_label(params.model).c_str());
        if (!params.model.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", params.path.empty() ? params.model.c_str()
                                                        : params.path.c_str());
        }
        if (open) {
            if (ImGui::Selectable("(none)", params.model.empty())) {
                update_config([&expert](Config& config) {
                    config.experts[expert.id].model.clear();
                    config.experts[expert.id].path.clear();
                });
            }
            for (const ModelFile& file : models_) {
                if (ImGui::Selectable((file.name + "   " + file.size_label()).c_str(),
                                      file.name == params.model)) {
                    update_config([&expert, &file](Config& config) {
                        config.experts[expert.id].model = file.name;
                    });
                }
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        if (ImGui::Button("Eject")) {
            eject = expert.id;
        }
        ImGui::Dummy(ImVec2(0, em(0.3F)));
        ImGui::PopID();
    }

    // Applied after the loop: removing a seat while iterating over the roster
    // it belongs to would invalidate the iterator.
    if (eject) {
        const std::string name = expert_label(config_.roster, *eject);
        update_config([&eject](Config& config) {
            std::string error;
            config.roster.remove(*eject, error);
            config.experts.erase(*eject);
            if (config.routing.default_expert == *eject) {
                config.routing.default_expert.clear();
            }
        });
        say(name + " has left the experts");
    }
}

}  // namespace crucible::gui
