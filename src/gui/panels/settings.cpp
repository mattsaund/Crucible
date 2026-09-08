// SPDX-License-Identifier: MIT
//
// The settings pages: General, Experts, Hardware, Tools, About.
//
// A list down the left and a page on the right, which is the shape every
// desktop application settles on because a single scrolling wall of switches
// cannot be navigated.
//
// Every change here goes through App::update_config, which writes the file and
// reconfigures the seats. Nothing on these pages edits the running state
// directly -- the config is the single place a setting lives, so the terminal
// program sees the same change on its next read.
#include "../app.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <filesystem>

#include "crucible/config/paths.hpp"
#include "crucible/runtime/devices.hpp"

#include "../theme.hpp"
#include "../widgets.hpp"

namespace crucible::gui {

void App::draw_settings() {
    // One list down the left and one page on the right. A single scrolling wall
    // of switches is what every desktop application starts with and none of
    // them keeps.
    ImGui::BeginChild("settings-nav", ImVec2(em(10.0F), 0), ImGuiChildFlags_Borders);
    const auto page = [this](const char* label, SettingsPage which) {
        if (ImGui::Selectable(label, settings_page_ == which, 0, ImVec2(0, em(1.5F)))) {
            settings_page_ = which;
        }
    };
    ImGui::Dummy(ImVec2(0, em(0.2F)));
    page("General",    SettingsPage::General);
    page("Experts",    SettingsPage::Experts);
    page("Generation", SettingsPage::Generation);
    page("Hardware",   SettingsPage::Hardware);
    page("Runtimes",   SettingsPage::Runtimes);
    page("Tools",      SettingsPage::Tools);
    page("About",      SettingsPage::About);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("settings-page", ImVec2(0, 0), ImGuiChildFlags_Borders);

    switch (settings_page_) {
        case SettingsPage::General: {
            title("General");

            section("DELEGATOR");
            wrapped(theme::kTextDim,
                    "A small model that reads your prompt and names the expert it "
                    "belongs to. It never answers; it only decides.");
            ImGui::SetNextItemWidth(em(20.0F));
            const bool open = ImGui::BeginCombo("Router model",
                                                model_label(config_.router.model).c_str());
            if (!config_.router.model.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", config_.router.path.c_str());
            }
            if (open) {
                if (ImGui::Selectable("(none) -- route on keywords instead",
                                      config_.router.model.empty())) {
                    update_config([](Config& config) { config.router.model.clear(); });
                }
                for (const ModelFile& file : models_) {
                    if (ImGui::Selectable((file.name + "   " + file.size_label()).c_str(),
                                          file.name == config_.router.model)) {
                        update_config([&file](Config& config) {
                            config.router.model = file.name;
                        });
                    }
                }
                ImGui::EndCombo();
            }

            bool keep = config_.routing.keep_delegator_loaded;
            if (ImGui::Checkbox("Keep the delegator in memory between prompts", &keep)) {
                update_config([keep](Config& config) {
                    config.routing.keep_delegator_loaded = keep;
                });
            }
            ImGui::SetItemTooltip(
                "Off -- the default -- frees it the moment it has routed, so the expert "
                "gets the whole card. On keeps it resident and charges every expert "
                "that follows its whole footprint.");

            float floor_value = config_.routing.min_confidence;
            ImGui::SetNextItemWidth(em(14.0F));
            if (ImGui::SliderFloat("Confidence floor", &floor_value, 0.0F, 1.0F, "%.2f")) {
                update_config([floor_value](Config& config) {
                    config.routing.min_confidence = floor_value;
                });
            }
            ImGui::SetItemTooltip(
                "Below this the delegator is treated as undecided. 0 disables the check.");

            section("MODELS");
            text_coloured(theme::kTextDim, "%s",
                          config_.resolved_models_dir().string().c_str());
            std::string dir = config_.models_dir;
            ImGui::SetNextItemWidth(-em(7.5F));
            if (ImGui::InputTextWithHint("##models-dir", "path to your GGUF files", &dir,
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                update_config([&dir](Config& config) { config.models_dir = dir; });
                refresh_models();
            }
            ImGui::SameLine();
            // Typing a path is fine when you know it; the button is for when
            // you do not, which is most of the time on a machine whose models
            // live somewhere a package manager chose.
            if (ImGui::Button("Browse...", ImVec2(-FLT_MIN, 0))) {
                open_browse(BrowseFor::ModelsDir);
            }
            if (ImGui::Button("Rescan", ImVec2(em(7.0F), 0))) {
                refresh_models();
                say("found " + std::to_string(models_.size()) + " GGUF files");
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset to default", ImVec2(em(11.0F), 0))) {
                update_config([](Config& config) { config.models_dir.clear(); });
                refresh_models();
                say("models directory reset to "
                    + config_.resolved_models_dir().string());
            }
            ImGui::SetItemTooltip("Back to %s",
                                  paths::models_dir().string().c_str());
            text_coloured(theme::kTextFaint, "%zu GGUF files here", models_.size());

            section("APPEARANCE");
            bool reasoning = config_.ui.show_reasoning;
            if (ImGui::Checkbox("Keep a thinking model's working on screen", &reasoning)) {
                update_config([reasoning](Config& config) {
                    config.ui.show_reasoning = reasoning;
                });
            }
            break;
        }

        case SettingsPage::Experts: {
            draw_expert_list();
            ImGui::Dummy(ImVec2(0, em(0.6F)));
            ImGui::Separator();
            section("DEFAULT EXPERT");
            wrapped(theme::kTextDim,
                    "Takes prompts the delegator could not place, and prompts routed to "
                    "a seat with no model. Any expert can play this part; without one, "
                    "an uncertain route is taken at face value.");
            const std::string current = config_.routing.default_expert.empty()
                ? std::string("(none)")
                : expert_label(config_.roster, config_.routing.default_expert);
            ImGui::SetNextItemWidth(em(20.0F));
            if (ImGui::BeginCombo("##default-expert", current.c_str())) {
                if (ImGui::Selectable("(none)", config_.routing.default_expert.empty())) {
                    update_config([](Config& config) {
                        config.routing.default_expert.clear();
                    });
                }
                for (const Expert& expert : config_.roster.experts()) {
                    if (ImGui::Selectable(expert.name.c_str(),
                                          config_.routing.default_expert == expert.id)) {
                        update_config([&expert](Config& config) {
                            config.routing.default_expert = expert.id;
                        });
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }

        case SettingsPage::Generation: draw_settings_generation(); break;
        case SettingsPage::Hardware:   draw_settings_hardware();   break;
        case SettingsPage::Runtimes:   draw_settings_runtimes();   break;

        case SettingsPage::Tools: {
            title("Tools");

            // No switches for reading, writing and running here any more. They
            // were three settings for one thing the user had already been asked
            // about -- trusting the folder -- and having said yes to that, they
            // then found nothing could be edited and no obvious reason why.
            // Trust is the decision; this page keeps the one number that is
            // genuinely a preference.
            section("PROJECT");
            text_coloured(theme::kFlame, "%s", store_->project().root.string().c_str());
            wrapped(theme::kTextDim,
                    "Experts can read, write and run things here because you trusted "
                    "this folder. Paths outside it are refused.");

            int timeout = config_.tools.workshop_timeout;
            ImGui::SetNextItemWidth(em(12.0F));
            if (ImGui::SliderInt("Command timeout (s)", &timeout, 5, 600)) {
                update_config([timeout](Config& config) {
                    config.tools.workshop_timeout = timeout;
                });
            }
            ImGui::SetItemTooltip("A build is minutes; a command still going after this "
                                  "is stuck.");

            section("WEB SEARCH");
            bool web = config_.tools.web_search;
            if (ImGui::Checkbox("Let experts look things up", &web)) {
                update_config([web](Config& config) { config.tools.web_search = web; });
            }
            ImGui::SetItemTooltip("The only thing Crucible sends off this machine.");
            if (config_.tools.web_search) {
                ImGui::SetNextItemWidth(em(14.0F));
                if (ImGui::BeginCombo("Provider", config_.tools.search_provider.c_str())) {
                    for (const char* which : {"wikipedia", "searxng", "brave"}) {
                        if (ImGui::Selectable(which,
                                              config_.tools.search_provider == which)) {
                            update_config([which](Config& config) {
                                config.tools.search_provider = which;
                            });
                        }
                    }
                    ImGui::EndCombo();
                }
                std::string endpoint = config_.tools.search_endpoint;
                ImGui::SetNextItemWidth(em(22.0F));
                if (ImGui::InputTextWithHint("Endpoint", "http://localhost:8888", &endpoint,
                                             ImGuiInputTextFlags_EnterReturnsTrue)) {
                    update_config([&endpoint](Config& config) {
                        config.tools.search_endpoint = endpoint;
                    });
                }

                // Brave is the only provider that takes one, so the box is only
                // worth showing for Brave. It is stored in the config as typed,
                // which the label says rather than leaving it to be discovered.
                if (config_.tools.search_provider == "brave") {
                    std::string key = config_.tools.search_api_key;
                    ImGui::SetNextItemWidth(em(22.0F));
                    if (ImGui::InputTextWithHint("API key", "stored in plain text", &key,
                                                 ImGuiInputTextFlags_Password |
                                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                        update_config([&key](Config& config) {
                            config.tools.search_api_key = key;
                        });
                    }
                    ImGui::SetItemTooltip(
                        "Written to config.json as typed. Anyone who can read your "
                        "config can read the key.");
                }

                int results = config_.tools.search_results;
                ImGui::SetNextItemWidth(em(10.0F));
                if (ImGui::SliderInt("Results", &results, 1, 20)) {
                    update_config([results](Config& config) {
                        config.tools.search_results = results;
                    });
                }
                ImGui::SetItemTooltip("How many hits are handed to the expert.");

                int rounds = config_.tools.search_rounds;
                ImGui::SetNextItemWidth(em(10.0F));
                if (ImGui::SliderInt("Rounds", &rounds, 1, 6)) {
                    update_config([rounds](Config& config) {
                        config.tools.search_rounds = rounds;
                    });
                }
                ImGui::SetItemTooltip(
                    "How many times one prompt may search before it has to answer. "
                    "A model that searches, reads, and searches again is usually "
                    "avoiding the question.");

                int search_timeout = config_.tools.search_timeout;
                ImGui::SetNextItemWidth(em(10.0F));
                if (ImGui::SliderInt("Search timeout (s)", &search_timeout, 2, 60)) {
                    update_config([search_timeout](Config& config) {
                        config.tools.search_timeout = search_timeout;
                    });
                }
            }
            break;
        }

        case SettingsPage::About: {
            title("Crucible " CRUCIBLE_VERSION);
            wrapped(theme::kTextDim,
                    "A local forge: experts on demand, projects that cook. This window "
                    "and the terminal program are the same engine -- same roster, same "
                    "cook loop, same config file.");

            section("FILES");
            text_coloured(theme::kTextDim, "config    %s",
                          paths::config_file().string().c_str());
            text_coloured(theme::kTextDim, "models    %s",
                          config_.resolved_models_dir().string().c_str());
            text_coloured(theme::kTextDim, "runtimes  %s",
                          paths::runtimes_dir().string().c_str());
            text_coloured(theme::kTextDim, "history   %s",
                          store_->project().dir.string().c_str());
            text_coloured(theme::kTextDim, "log       %s",
                          paths::log_file().string().c_str());

            section("TRUSTED FOLDERS");
            wrapped(theme::kTextDim,
                    "Crucible asks once per directory before it will read or write "
                    "there. These are the ones you have said yes to.");
            for (const std::filesystem::path& entry : trust_.entries()) {
                text_coloured(theme::kTextFaint, "%s", entry.string().c_str());
            }
            break;
        }
    }
    ImGui::EndChild();
}

}  // namespace crucible::gui
