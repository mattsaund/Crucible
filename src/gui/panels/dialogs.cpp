// SPDX-License-Identifier: MIT
//
// The three modals: new expert, choose a folder, and the folder-trust question.
//
// A modal is used where the answer changes what the rest of the window means
// -- a different working directory is a different history, journal and
// project root -- and nowhere else.
//
// The folder browser is one dialog serving two questions, because they are the
// same question: which directory. Where the models are, and where Crucible
// works.
//
// The trust dialog is the one that matters: it asks before Crucible is allowed
// to read and write a directory, and it goes through the same store the
// terminal program uses, so a directory trusted in one face is trusted in the
// other.
#include "../app.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <filesystem>
#include <system_error>

#include "crucible/config/paths.hpp"
#include "crucible/util/format.hpp"

#include "../theme.hpp"
#include "../widgets.hpp"

namespace crucible::gui {

void App::draw_new_expert_modal() {
    if (expert_modal_open_) {
        ImGui::OpenPopup("New expert");
        expert_modal_open_ = false;
    }

    ImGui::SetNextWindowSize(ImVec2(em(30.0F), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("New expert", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    // Two boxes and nothing else. The id, the chip, the keyword set and the
    // worked examples the delegator routes on are all derived or generated,
    // because those are things a person should not have to invent.
    wrapped(theme::kTextDim,
            "A name, and what it is trained in. Crucible works out the rest, and the "
            "delegator writes its own example questions once it is loaded.");
    ImGui::Dummy(ImVec2(0, em(0.5F)));

    text_colored(theme::kTextFaint, "Expert name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##name", "Rust Async, Tax Law, Kubernetes", &new_expert_name_);

    ImGui::Dummy(ImVec2(0, em(0.4F)));
    text_colored(theme::kTextFaint, "Describe what the expert is trained in");
    ImGui::InputTextMultiline("##blurb", &new_expert_blurb_, ImVec2(-FLT_MIN, em(5.2F)));
    text_colored(theme::kTextFaint,
                  "The delegator routes on this, so name the things it should take.");

    if (!expert_error_.empty()) {
        ImGui::Dummy(ImVec2(0, em(0.4F)));
        wrapped(theme::kError, expert_error_);
    }

    ImGui::Dummy(ImVec2(0, em(0.6F)));
    if (ImGui::Button("Add expert", ImVec2(em(8.0F), 0))) {
        Expert expert;
        expert.name  = format::trim(new_expert_name_);
        expert.blurb = format::trim(new_expert_blurb_);

        Config      edited = config_;
        std::string error;
        if (!edited.roster.add(expert, error)) {
            // The dialog stays open with what was typed still in it: a name
            // collision is fixed by editing the name, not by typing the
            // description again.
            expert_error_ = error;
        } else {
            const ExpertId id = make_expert_id(expert.name);
            edited.experts[id] = ModelParams{};
            update_config([&edited](Config& config) { config = edited; });
            engine_->write_examples(id);
            say(expert.name + " has joined the experts");
            new_expert_name_.clear();
            new_expert_blurb_.clear();
            expert_error_.clear();
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em(5.6F), 0))) {
        expert_error_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::draw_browse_modal() {
    const bool for_models = browse_for_ == BrowseFor::ModelsDir;
    const char* const kTitle = "Choose a folder";

    if (browse_modal_open_) {
        ImGui::OpenPopup(kTitle);
        browse_modal_open_ = false;
    }

    ImGui::SetNextWindowSize(ImVec2(em(38.0F), em(30.0F)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    // Wrapped, not printed: both of these are longer than the dialog is wide,
    // and an unwrapped line is simply cut off at the edge mid-word.
    wrapped(theme::kTextDim, for_models
        ? "Where your GGUF files are. Crucible reads this directory; it never "
          "writes to it and never downloads into it."
        : "The directory Crucible works in: its history, its cook journal and "
          "the folder experts read and write inside.");
    ImGui::Dummy(ImVec2(0, em(0.3F)));

    // A browser rather than a native file dialog. Crucible has no toolkit to
    // ask for one, and a directory list is what this needs anyway: you are
    // choosing a folder to work in, not a file to load.
    ImGui::SetNextItemWidth(-em(9.0F));
    if (ImGui::InputText("##path", &browse_text_, ImGuiInputTextFlags_EnterReturnsTrue)) {
        const std::filesystem::path typed = paths::expand_user(browse_text_);
        std::error_code ec;
        if (std::filesystem::is_directory(typed, ec)) {
            browse_ = typed;
            project_error_.clear();
        } else {
            project_error_ = browse_text_ + " is not a directory";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Up", ImVec2(em(3.2F), 0)) && browse_.has_parent_path()) {
        browse_      = browse_.parent_path();
        browse_text_ = browse_.string();
    }
    ImGui::SameLine();
    if (ImGui::Button("Home", ImVec2(-FLT_MIN, 0))) {
        browse_      = paths::expand_user("~");
        browse_text_ = browse_.string();
    }

    text_colored(theme::kTextFaint, "%s", browse_.string().c_str());

    // Sized to what is left rather than to a fixed height: the "create folder"
    // row that used to sit under it is gone, and a list that kept its old
    // height would just leave a strip of nothing where it was.
    const float reserve = em(3.4F) + (project_error_.empty() ? 0.0F : em(1.8F));
    ImGui::BeginChild("dirs", ImVec2(0, -reserve), ImGuiChildFlags_Borders);
    for (const std::filesystem::path& entry : subdirectories(browse_)) {
        // string(), not c_str(): on Windows a path is wide, and PushID would
        // quietly take the pointer rather than the name.
        ImGui::PushID(entry.string().c_str());
        if (ImGui::Selectable((entry.filename().string() + "/").c_str())) {
            browse_      = entry;
            browse_text_ = browse_.string();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // There is no "create project" here any more. It made this dialog two
    // things -- a browser and a folder-maker -- for a program that has no
    // notion of a project beyond "the directory it is pointed at". A new
    // project is a new folder, made wherever you make folders, and then opened
    // like any other.
    if (!project_error_.empty()) {
        wrapped(theme::kError, project_error_);
    }

    ImGui::Separator();
    if (ImGui::Button(for_models ? "Use this folder" : "Open this folder",
                      ImVec2(em(12.0F), 0))) {
        const std::filesystem::path chosen = browse_;
        ImGui::CloseCurrentPopup();
        if (for_models) {
            update_config([&chosen](Config& config) {
                config.models_dir = chosen.string();
            });
            refresh_models();
            say("models directory: " + chosen.string() + " -- "
                + std::to_string(models_.size()) + " GGUF files");
        } else {
            open_project(chosen);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em(5.6F), 0))) {
        project_error_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::draw_trust_modal() {
    if (pending_trust_ && !ImGui::IsPopupOpen("Trust this folder?")) {
        ImGui::OpenPopup("Trust this folder?");
    }

    ImGui::SetNextWindowSize(ImVec2(em(32.0F), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Trust this folder?", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    if (!pending_trust_) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    wrapped(theme::kText, pending_trust_->string());
    ImGui::Dummy(ImVec2(0, em(0.4F)));
    // This is the whole permission now. There used to be two more switches
    // behind it, which meant this question could be answered yes and nothing
    // could still be edited; now yes means yes, so it has to say what it grants
    // without hedging.
    wrapped(theme::kTextDim,
            "Crucible will read, write and run commands in this folder, and keep its "
            "history. Paths outside it are refused -- but a command it runs is a "
            "command, and can reach whatever you can.");
    ImGui::Dummy(ImVec2(0, em(0.6F)));

    if (ImGui::Button("Trust and open", ImVec2(em(11.0F), 0))) {
        const std::filesystem::path root = *pending_trust_;
        pending_trust_.reset();
        trust_.trust(root);
        ImGui::CloseCurrentPopup();
        open_project(root);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em(5.6F), 0))) {
        // Declining the folder the window opened in is different from declining
        // one picked from the sidebar: there is no trusted project underneath to
        // fall back to, so canceling would leave the user sitting in a
        // directory they just refused. Offer the picker instead of nothing.
        const bool was_the_open_project = *pending_trust_ == store_->project().root;
        pending_trust_.reset();
        ImGui::CloseCurrentPopup();
        if (was_the_open_project) {
            say("not trusted -- choose a folder to work in");
            open_browse(BrowseFor::Project);
        }
    }
    ImGui::EndPopup();
}

}  // namespace crucible::gui
