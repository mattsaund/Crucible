// SPDX-License-Identifier: MIT
//
// The Create tab: fine-tuning a model rather than running one.
//
// Chat and Cook use models. This is where a specialized one comes from: a name,
// a base model, the data to specialize it on, the tools it should learn to
// reach for, and a file at the end that any llama.cpp or MLX program can load.
//
// Fine-tuning, not training from nothing. Pretraining even a small model is
// weeks of rented GPUs; an adapter over a model that already speaks is an
// evening on a card somebody owns, and it is what "an expert in one subject"
// actually takes.
//
// And a roster of them is the point. Crucible routes a question to the expert
// whose subject it belongs to, which is only as good as the experts there are
// to choose between: five fine-tunes -- math, physics, programming, chemistry,
// biology -- are five seats the delegator can pick from, and each one is a
// small model that fits beside the others rather than one big model that knows
// a little about everything. So several recipes live here at once.
//
// The shape is a rail of steps down the left and one step at a time on the
// right, for the same reason Settings is: the whole of it does not fit on a
// screen, and a wall of fields is not a flow anybody follows. Each step says
// what it is for in a line, because most people arriving here have not done
// this before -- that is the point of it.
//
// What is built: everything up to the run. A recipe is assembled, saved, and
// checked against the machine it would train on. The trainer, the test bench
// and the export are the next three pieces, and the steps for them say so
// rather than offering a button that does nothing.
#include "../app.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <thread>
#include <utility>

#include "crucible/runtime/devices.hpp"
#include "crucible/util/format.hpp"

#include "../theme.hpp"
#include "../widgets.hpp"

namespace crucible::gui {
namespace {

struct Step {
    const char* label;
    const char* blurb;
};

/// The flow, in the order it is asked for. A step is not a page of settings:
/// it is one decision, with the reason for it written next to it.
constexpr std::array<Step, 8> kSteps{{
    {"Name",       "What this expert is for. Everything else is downstream of knowing that."},
    {"Base model", "What it starts from. Fine-tuning specializes a model that already "
                   "speaks; it does not teach one to speak from nothing."},
    {"Data",       "What it learns from. A few megabytes of the right text beats a few "
                   "gigabytes of the wrong."},
    {"Tools",      "What it learns to reach for: reading files, running commands, searching."},
    {"Target",     "How it trains and what comes out: the method, the size, the format."},
    {"Train",      "The run itself."},
    {"Test",       "Talk to it before you keep it."},
    {"Export",     "A file you can take anywhere."},
}};

/// What the GPUs have between them. The estimate uses it because training
/// happens on the card when there is one; with no card at all the step says so
/// rather than quietly measuring something else.
std::uint64_t vram_here() {
    std::uint64_t total = 0;
    for (const ComputeDevice& gpu : gpu_devices()) {
        total += gpu.memory_total;
    }
    return total;
}

const char* const kQuantizations[] = {"Q4_K_M", "Q5_K_M", "Q6_K", "Q8_0", "F16"};

}  // namespace

std::optional<lab::Asset> App::draw_hub_picker(lab::hub::Kind kind, const char* placeholder) {
    std::optional<lab::Asset> picked;

    ImGui::SetNextItemWidth(em(22.0F));
    const bool entered = ImGui::InputTextWithHint("##hub-query", placeholder, &lab_query_,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool busy = lab_searching_ != nullptr;
    ImGui::BeginDisabled(busy || lab_query_.empty());
    const bool go = ImGui::Button("Search");
    ImGui::EndDisabled();

    if ((entered || go) && !busy && !lab_query_.empty()) {
        lab_error_.clear();
        lab_results_.clear();
        // On a thread, because the hub takes a second or two to answer and a
        // window that stops drawing for two seconds reads as a crash. The
        // worker fills the shared box and wakes the loop; the next frame picks
        // it up.
        auto search = std::make_shared<LabSearch>();
        lab_searching_ = search;
        std::thread([search, kind, query = lab_query_]() {
            std::vector<lab::hub::Item> items;
            std::string                 error;
            lab::hub::search(kind, query, 25, items, error);
            {
                const std::lock_guard<std::mutex> lock(search->mutex);
                search->items = std::move(items);
                search->error = std::move(error);
                search->done  = true;
            }
            glfwPostEmptyEvent();
        }).detach();
    }

    if (lab_searching_ != nullptr) {
        const std::lock_guard<std::mutex> lock(lab_searching_->mutex);
        if (lab_searching_->done) {
            lab_results_ = std::move(lab_searching_->items);
            lab_error_   = std::move(lab_searching_->error);
            if (lab_results_.empty() && lab_error_.empty()) {
                lab_error_ = "nothing on Huggingface matched that";
            }
        }
    }
    if (lab_searching_ != nullptr) {
        const std::lock_guard<std::mutex> lock(lab_searching_->mutex);
        if (lab_searching_->done) {
            lab_searching_.reset();
        }
    }

    if (lab_searching_ != nullptr) {
        text_colored(theme::kTextDim, "asking Huggingface...");
    }
    if (!lab_error_.empty()) {
        text_colored(theme::kFlameBright, "%s", lab_error_.c_str());
    }

    if (!lab_results_.empty()) {
        ImGui::Dummy(ImVec2(0, em(0.3F)));
        ImGui::BeginChild("hub-results", ImVec2(0, em(14.0F)), ImGuiChildFlags_Borders);
        for (std::size_t i = 0; i < lab_results_.size(); ++i) {
            const lab::hub::Item& item = lab_results_[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Button("Use")) {
                lab::Asset asset;
                asset.source = lab::Source::Hub;
                asset.id     = item.id;
                asset.label  = item.id;
                picked       = asset;
            }
            ImGui::SameLine();
            text_colored(item.gated ? theme::kTextFaint : theme::kText, "%s", item.id.c_str());
            ImGui::SameLine();
            std::string note = format::number(static_cast<double>(item.downloads) / 1000.0, 0)
                             + "k downloads";
            if (item.parameters_b > 0.0) {
                note += "  ·  " + format::number(item.parameters_b, 1) + "B";
            }
            if (item.gated) {
                // Gated repositories need a browser and an accepted license.
                // Saying so here saves a download that would 401 an hour later.
                note += "  ·  gated: accept its license on Huggingface first";
            }
            text_colored(theme::kTextFaint, "%s", note.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    return picked;
}

void App::draw_create(const Snapshot& snapshot) {
    (void)snapshot;

    const auto save_now = [this]() {
        std::string error;
        if (!lab_recipe_.name.empty() && !lab::save(lab_recipe_, error)) {
            lab_error_ = error;
        }
    };

    // Read once: the disk is the list, and rereading it every frame to draw
    // four names would be four file reads at eleven frames a second.
    if (!lab_saved_read_) {
        lab_saved_      = lab::saved_recipes();
        lab_saved_read_ = true;
    }
    const auto refresh_saved = [this]() { lab_saved_ = lab::saved_recipes(); };

    // --- the rail ----------------------------------------------------------
    ImGui::BeginChild("create-rail", ImVec2(em(12.0F), 0), ImGuiChildFlags_Borders);
    ImGui::Dummy(ImVec2(0, em(0.2F)));
    const std::vector<std::string> gaps = lab::missing(lab_recipe_);

    // The models being built, because the tab is for a roster of them.
    section("MODELS");
    for (const lab::Recipe& saved : lab_saved_) {
        ImGui::PushID(saved.id.c_str());
        const bool current = saved.id == lab_recipe_.id && !saved.id.empty();
        if (ImGui::Selectable(saved.name.c_str(), current, 0, ImVec2(0, em(1.4F)))
            && !current) {
            save_now();          // what is open now keeps what was typed into it
            lab_recipe_ = saved;
            lab_step_   = 0;
            lab_query_.clear();
            lab_results_.clear();
            lab_error_.clear();
        }
        ImGui::PopID();
    }
    if (ImGui::Button("New")) {
        save_now();
        lab_recipe_ = lab::Recipe{};
        lab_step_   = 0;
        lab_query_.clear();
        lab_results_.clear();
        lab_error_.clear();
        refresh_saved();
    }
    ImGui::SetItemTooltip("Start another expert. The one open now is kept.");

    ImGui::Dummy(ImVec2(0, em(0.4F)));
    section("STEPS");
    for (std::size_t i = 0; i < kSteps.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        // A tick on what is settled. The rail is the answer to "how much of
        // this is left", which is the question anybody halfway through a form
        // is actually asking.
        const bool done = (i == 0 && !lab_recipe_.name.empty())
                       || (i == 1 && lab_recipe_.base.chosen())
                       || (i == 2 && !lab_recipe_.data.empty())
                       || (i == 3 && !lab_recipe_.tools.empty())
                       || (i == 4 && lab_recipe_.parameters_b > 0.0);
        if (ImGui::Selectable(kSteps[i].label, lab_step_ == static_cast<int>(i), 0,
                              ImVec2(0, em(1.5F)))) {
            if (lab_step_ != static_cast<int>(i)) {
                // Each step searches for something else. Models left in the box
                // when the data step opens are not results, they are litter.
                lab_step_ = static_cast<int>(i);
                lab_query_.clear();
                lab_results_.clear();
                lab_error_.clear();
                lab_local_path_.clear();
            }
        }
        if (done) {
            ImGui::SameLine();
            text_colored(theme::kFlame, "done");
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("create-step", ImVec2(0, 0), ImGuiChildFlags_Borders);

    const Step& step = kSteps[static_cast<std::size_t>(std::clamp(lab_step_, 0,
                                     static_cast<int>(kSteps.size()) - 1))];
    title(step.label);
    wrapped(theme::kTextDim, step.blurb);
    if (!lab_error_.empty() && lab_step_ != 1 && lab_step_ != 2 && lab_step_ != 3) {
        // The picker prints its own; everywhere else this is the only place a
        // failed save would otherwise be seen, which is to say not at all.
        text_colored(theme::kFlameBright, "%s", lab_error_.c_str());
    }
    ImGui::Dummy(ImVec2(0, em(0.5F)));

    switch (lab_step_) {
        case 0: {
            section("NAME");
            ImGui::SetNextItemWidth(em(22.0F));
            if (ImGui::InputTextWithHint("##lab-name", "Kitchen Physicist", &lab_recipe_.name)) {
                lab_recipe_.id = lab::slug_of(lab_recipe_.name);
            }
            if (!lab_recipe_.id.empty()) {
                text_colored(theme::kTextFaint, "saved as %s", lab_recipe_.id.c_str());
            }
            section("DESCRIPTION (OPTIONAL)");
            ImGui::SetNextItemWidth(em(30.0F));
            ImGui::InputTextWithHint("##lab-purpose", "answers questions about induction hobs",
                                     &lab_recipe_.purpose);
            ImGui::Dummy(ImVec2(0, em(0.4F)));
            ImGui::BeginDisabled(lab_recipe_.name.empty());
            if (ImGui::Button("Save")) {
                save_now();
                refresh_saved();
            }
            ImGui::EndDisabled();
            break;
        }

        case 1: {
            section("FROM HUGGINGFACE");
            if (const std::optional<lab::Asset> picked =
                    draw_hub_picker(lab::hub::Kind::Model, "llama 3.2 1b, qwen 0.5b, smollm...")) {
                lab_recipe_.base         = *picked;
                lab_recipe_.parameters_b = lab::hub::parameters_from_name(picked->id);
                for (const lab::hub::Item& item : lab_results_) {
                    if (item.id == picked->id && item.parameters_b > 0.0) {
                        lab_recipe_.parameters_b = item.parameters_b;
                    }
                }
                save_now();
            }

            section("OR A FILE ON THIS MACHINE");
            wrapped(theme::kTextDim, "A GGUF or a directory of weights you already have.");
            ImGui::SetNextItemWidth(em(28.0F));
            ImGui::InputTextWithHint("##lab-base-local", "/models/llama-3.2-1b.gguf",
                                     &lab_local_path_);
            ImGui::SameLine();
            ImGui::BeginDisabled(lab_local_path_.empty());
            if (ImGui::Button("Use this")) {
                lab::Asset asset;
                asset.source = lab::Source::Local;
                asset.id     = lab_local_path_;
                asset.path   = lab_local_path_;
                asset.label  = std::filesystem::path(lab_local_path_).filename().string();
                lab_recipe_.base = asset;
                if (lab_recipe_.parameters_b <= 0.0) {
                    lab_recipe_.parameters_b = lab::hub::parameters_from_name(asset.label);
                }
                lab_local_path_.clear();
                save_now();
            }
            ImGui::EndDisabled();

            if (lab_recipe_.base.chosen()) {
                ImGui::Dummy(ImVec2(0, em(0.5F)));
                section("CHOSEN");
                text_colored(theme::kText, "%s", lab_recipe_.base.id.c_str());
                if (lab_recipe_.parameters_b > 0.0) {
                    text_colored(theme::kTextFaint, "%s billion parameters",
                                 format::number(lab_recipe_.parameters_b, 1).c_str());
                }
                const std::uint64_t vram = vram_here();
                const lab::Fit fit =
                    lab::estimate_fit(lab_recipe_.parameters_b, lab_recipe_.method, vram, 0);
                if (vram == 0) {
                    // An empty device list means no GPU runtime is installed, which
                    // is not the same as no GPU: the cards are there and nothing can
                    // see them yet.
                    text_colored(theme::kTextDim,
                                 "no GPU runtime installed -- add one in Settings, "
                                 "Runtimes, and this will say whether a card can hold it");
                } else {
                    text_colored(fit.possible ? theme::kText : theme::kFlameBright,
                                 "%s of %s  --  %s", format::bytes(fit.needed).c_str(),
                                 format::bytes(fit.have).c_str(), fit.note.c_str());
                }
            }
            break;
        }

        case 2:
        case 3: {
            const bool tools = lab_step_ == 3;
            std::vector<lab::Asset>& into = tools ? lab_recipe_.tools : lab_recipe_.data;
            if (tools) {
                wrapped(theme::kTextDim,
                        "Tool use is learned from examples of it: transcripts where the model "
                        "asks to read a file or run a command and uses what comes back. "
                        "Crucible's own tool protocol is one of these.");
            }
            section("FROM HUGGINGFACE");
            if (const std::optional<lab::Asset> picked = draw_hub_picker(
                    lab::hub::Kind::Dataset,
                    tools ? "function calling, tool use, agent traces..."
                          : "wikitext, textbooks, your subject...")) {
                into.push_back(*picked);
                save_now();
            }

            section("OR FILES ON THIS MACHINE");
            wrapped(theme::kTextDim,
                    "A file or a folder: plain text, JSONL, markdown. This is the part that "
                    "makes the model yours rather than another copy of what is already public.");
            ImGui::SetNextItemWidth(em(28.0F));
            ImGui::InputTextWithHint("##lab-data-local", "/home/you/notes", &lab_local_path_);
            ImGui::SameLine();
            ImGui::BeginDisabled(lab_local_path_.empty());
            if (ImGui::Button("Add")) {
                lab::Asset asset;
                asset.source = lab::Source::Local;
                asset.id     = lab_local_path_;
                asset.path   = lab_local_path_;
                asset.label  = std::filesystem::path(lab_local_path_).filename().string();
                into.push_back(asset);
                lab_local_path_.clear();
                save_now();
            }
            ImGui::EndDisabled();

            if (!into.empty()) {
                ImGui::Dummy(ImVec2(0, em(0.5F)));
                section("CHOSEN");
                for (std::size_t i = 0; i < into.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Button("Remove")) {
                        into.erase(into.begin() + static_cast<long>(i));
                        save_now();
                        ImGui::PopID();
                        break;
                    }
                    ImGui::SameLine();
                    text_colored(theme::kText, "%s", into[i].id.c_str());
                    ImGui::SameLine();
                    text_colored(theme::kTextFaint, "%s",
                                 into[i].source == lab::Source::Local ? "on this machine"
                                                                        : "Huggingface");
                    ImGui::PopID();
                }
            }
            break;
        }

        case 4: {
            section("METHOD");
            const std::uint64_t vram = vram_here();
            const auto method_row = [&](const char* label, lab::Method method,
                                        const char* explain) {
                const bool on = lab_recipe_.method == method;
                if (ImGui::RadioButton(label, on)) {
                    lab_recipe_.method = method;
                    save_now();
                }
                ImGui::SameLine();
                const lab::Fit fit =
                    lab::estimate_fit(lab_recipe_.parameters_b, method, vram, 0);
                text_colored(fit.possible ? theme::kTextDim : theme::kTextFaint, "%s", explain);
                if (lab_recipe_.parameters_b > 0.0 && vram > 0) {
                    text_colored(fit.possible ? theme::kText : theme::kFlameBright,
                                 "    needs about %s, this machine has %s",
                                 format::bytes(fit.needed).c_str(),
                                 format::bytes(fit.have).c_str());
                }
            };
            method_row("QLoRA", lab::Method::Qlora,
                       "An adapter over a four-bit base. What fits a big model on one card.");
            method_row("LoRA", lab::Method::Lora,
                       "An adapter over a sixteen-bit base: a shade better, where it fits.");

            section("SIZE");
            wrapped(theme::kTextDim,
                    "What the exported file is quantized to. Smaller is faster and "
                    "coarser; the sizes are for the base model chosen.");
            for (const char* quant : kQuantizations) {
                const bool on = lab_recipe_.quantization == quant;
                ImGui::PushID(quant);
                if (ImGui::RadioButton(quant, on)) {
                    lab_recipe_.quantization = quant;
                    save_now();
                }
                if (lab_recipe_.parameters_b > 0.0) {
                    ImGui::SameLine();
                    text_colored(theme::kTextFaint, "about %s",
                                 format::bytes(lab::export_bytes(lab_recipe_.parameters_b, quant))
                                     .c_str());
                }
                ImGui::PopID();
            }

            section("THE RUN");
            ImGui::SetNextItemWidth(em(14.0F));
            if (ImGui::SliderInt("Passes over the data", &lab_recipe_.epochs, 1, 10)) {
                save_now();
            }
            ImGui::SetNextItemWidth(em(14.0F));
            if (ImGui::SliderInt("Context", &lab_recipe_.context, 256, 4096)) {
                save_now();
            }
            ImGui::SetNextItemWidth(em(14.0F));
            if (ImGui::SliderFloat("Learning rate", &lab_recipe_.learning_rate, 1e-6F, 1e-3F,
                                   "%.1e", ImGuiSliderFlags_Logarithmic)) {
                save_now();
            }

            section("FORMAT");
            if (ImGui::RadioButton("GGUF", lab_recipe_.format == lab::Export::Gguf)) {
                lab_recipe_.format = lab::Export::Gguf;
                save_now();
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("MLX", lab_recipe_.format == lab::Export::Mlx)) {
                lab_recipe_.format = lab::Export::Mlx;
                save_now();
            }
            ImGui::SameLine();
            text_colored(theme::kTextFaint, "GGUF runs anywhere; MLX is for Apple silicon");
            break;
        }

        default: {
            // Train, Test and Export. Said plainly rather than drawn as buttons
            // that do nothing: the recipe is real and the run is not, and a
            // disabled button with no explanation is worse than a sentence.
            section("WHAT IS READY");
            if (gaps.empty()) {
                text_colored(theme::kText, "%s", "The recipe is complete.");
            } else {
                for (const std::string& gap : gaps) {
                    text_colored(theme::kFlameBright, "still needs %s", gap.c_str());
                }
            }
            if (lab_step_ == 7) {
                ImGui::Dummy(ImVec2(0, em(0.5F)));
                section("AND THEN A SEAT");
                wrapped(theme::kTextDim,
                        "Crucible routes by subject: the delegator reads each expert's "
                        "description and picks the one a question belongs to. Exporting "
                        "puts this model on the roster as a seat of its own, which is how "
                        "five fine-tunes become five experts it can choose between.");
                text_colored(theme::kText, "seat: %s",
                             lab_recipe_.name.empty() ? "(unnamed)" : lab_recipe_.name.c_str());
                if (lab_recipe_.purpose.empty()) {
                    // The blurb is the single field routing depends on. A seat
                    // with only a name to go on is one the delegator cannot tell
                    // from its neighbors.
                    text_colored(theme::kFlameBright,
                                 "no description: the delegator would have only the name to "
                                 "route on");
                } else {
                    text_colored(theme::kTextFaint, "%s", lab_recipe_.purpose.c_str());
                }
                ImGui::BeginDisabled(true);
                ImGui::Button("Add as an expert");
                ImGui::EndDisabled();
                ImGui::SetItemTooltip("Once it has been fine-tuned and exported.");
            }

            ImGui::Dummy(ImVec2(0, em(0.5F)));
            section("NOT BUILT YET");
            wrapped(theme::kTextDim,
                    "The trainer, the test bench and the export are the next three pieces. "
                    "Everything up to here -- what to start from, what to learn from, what it "
                    "should fit in -- is real and is saved.");
            ImGui::Dummy(ImVec2(0, em(0.4F)));
            wrapped(theme::kTextDim,
                    "Both methods train an adapter, which is PyTorch work rather than "
                    "llama.cpp work, so the run will want a Python environment. Crucible "
                    "will install and keep that itself, the way it builds a GPU runtime "
                    "now -- nothing to set up by hand.");
            break;
        }
    }

    ImGui::EndChild();
}

}  // namespace crucible::gui
