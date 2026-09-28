// SPDX-License-Identifier: MIT
//
// The Create tab: the experts you have made, and the making of another one.
//
// Chat and Cook use models. This is where a specialized one comes from: a
// name, a base model, the data to specialize it on, the tools it should learn
// to reach for, and a file at the end that any llama.cpp or MLX program can
// load.
//
// Fine-tuning, not training from nothing. Pretraining even a small model is
// weeks of rented GPUs; an adapter over a model that already speaks is an
// evening on a card somebody owns, and it is what "an expert in one subject"
// actually takes.
//
// And a roster of them is the point. Crucible routes a question to the expert
// whose subject it belongs to, which is only as good as the experts there are
// to choose between: five fine-tunes -- math, physics, programming, chemistry,
// biology -- are five seats the delegator can pick from, each small enough to
// sit beside the others.
//
// So this tab is a list before it is a form. Three screens:
//
//   the list      what has been made, what each one is, and where it got to
//   the page      one expert: its specs, its progress, and what can be done
//   the wizard    a modal that collects a recipe and starts its run
//
// plus the test window, which is a modal because talking to a candidate is
// something you do to decide whether to keep it, not a place to browse to.
//
// The form used to be the whole tab, a rail of eight steps with a list of
// names bolted to the top of it. That reads as "fill this in" on every visit,
// which is wrong after the first: most visits are to look at the roster.
//
// What is built and what is not, said plainly rather than drawn as buttons
// that do nothing: a recipe is assembled, saved and checked against the
// machine it would train on, and the model that comes out is tested, kept and
// seated from here. The fine-tune itself is the gap -- it wants a Python
// environment Crucible does not install -- so a run is queued rather than
// started, and the training page says so. A file trained elsewhere can be
// attached to its recipe, which is what makes the rest of the flow real today.
#include "../app.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "crucible/runtime/devices.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/platform.hpp"

#include "../markdown_view.hpp"
#include "../theme.hpp"
#include "../widgets.hpp"

namespace crucible::gui {
namespace {

struct Step {
    const char* label;
    const char* blurb;
};

/// The wizard, in the order it asks. One line each, and the line says what the
/// step takes rather than why the step exists -- the chips across the top
/// already say where you are, and the controls say what they do.
///
/// Five steps that collect something and one that shows what they collected.
/// Train, Test and Export used to be steps six, seven and eight; they are not
/// things you fill in, they are things that happen to a recipe afterwards, and
/// they live on the expert's own page now.
constexpr std::array<Step, 6> kSteps{{
    {"Name",       "What this expert is for."},
    {"Base model", "What it starts from."},
    {"Data",       "What it learns from."},
    {"Tools",      "What it learns to reach for."},
    {"Target",     "How it trains, and what comes out."},
    {"Review",     "What the run will be."},
}};

constexpr int kStepTarget = 4;
constexpr int kStepReview = 5;

/// The seat a candidate is tested on.
///
/// One id for every test rather than one per recipe: only one test window can
/// be open, and reusing the id means a window closed the hard way cannot leave
/// a second stale seat sitting behind the first.
constexpr const char* kTestSeat = "lab-test";

const char* const kQuantizations[] = {"Q4_K_M", "Q5_K_M", "Q6_K", "Q8_0", "F16"};

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

/// The color a stage is written in. Finished is the flame because it is the
/// one that ends somewhere; a draft is faint because it is not a thing yet.
ImU32 stage_color(lab::Stage stage) {
    switch (stage) {
        case lab::Stage::Finished: return theme::kFlame;
        case lab::Stage::Testing:  return theme::kFlameBright;
        case lab::Stage::Training: return theme::kTextDim;
        case lab::Stage::Draft:    break;
    }
    return theme::kTextFaint;
}

std::string method_text(lab::Method method) {
    return method == lab::Method::Lora ? "LoRA" : "QLoRA";
}

/// The specs as one line, for the list.
///
/// What tells one expert from another at a glance and nothing else. Two
/// recipes that differ only in learning rate are not told apart by a list;
/// they are told apart by opening one.
std::string spec_line(const lab::Recipe& recipe) {
    std::vector<std::string> parts;
    if (recipe.base.chosen()) {
        parts.push_back(recipe.base.label.empty() ? recipe.base.id : recipe.base.label);
    }
    if (recipe.parameters_b > 0.0) {
        parts.push_back(format::number(recipe.parameters_b, 1) + "B");
    }
    parts.push_back(method_text(recipe.method));
    parts.push_back(recipe.quantization);
    if (!recipe.data.empty()) {
        parts.push_back(std::to_string(recipe.data.size())
                        + (recipe.data.size() == 1 ? " data set" : " data sets"));
    }
    if (parts.empty()) {
        return "nothing chosen yet";
    }
    std::string line = parts.front();
    for (std::size_t i = 1; i < parts.size(); ++i) {
        line += "  \xc2\xb7  " + parts[i];
    }
    return line;
}

/// A learning rate the way papers write it: 1.0e-05.
///
/// Worth a function because the obvious thing -- scale by a million and print
/// the mantissa -- renders 1e-5 as "10.0e-06", which is the same number and
/// not the same string, and nobody comparing two runs wants to do that in
/// their head.
std::string rate_text(float rate) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%.1e", static_cast<double>(rate));
    return buffer;
}

/// A date as it is read rather than as it is stored. Empty for "never".
std::string when_text(std::int64_t epoch) {
    if (epoch <= 0) {
        return {};
    }
    const std::tm parts = util::local_time(static_cast<std::time_t>(epoch));
    char           buffer[48];
    if (std::strftime(buffer, sizeof(buffer), "%d %b %Y at %H:%M", &parts) == 0) {
        return {};
    }
    return buffer;
}

/// How long ago `epoch` was, to one unit. Nobody reading "queued 4 hours ago"
/// wanted the minutes as well.
std::string since_text(std::int64_t epoch) {
    const std::int64_t seconds = std::max<std::int64_t>(lab::now_seconds() - epoch, 0);
    const auto         count   = [](std::int64_t n, const char* unit) {
        return std::to_string(n) + " " + unit + (n == 1 ? "" : "s");
    };
    if (seconds < 90)     { return "less than a minute"; }
    if (seconds < 5400)   { return count(seconds / 60, "minute"); }
    if (seconds < 172800) { return count(seconds / 3600, "hour"); }
    return count(seconds / 86400, "day");
}

/// Bytes of a file that may not be there. Zero for one that is not.
std::uint64_t file_bytes(const std::string& path) {
    if (path.empty()) {
        return 0;
    }
    std::error_code ec;
    const auto      size = std::filesystem::file_size(std::filesystem::path(path), ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

/// A clickable block with its own background, for one row of the list.
///
/// A Selectable cannot do this: the rows are three lines of differently
/// colored text, and a Selectable takes one string. So the hit target is an
/// invisible button the width of the list, the plate is drawn behind it, and
/// the caller writes the three lines over the top -- later submissions draw
/// later, which is what puts the text above its own background.
struct Card {
    ImVec2 origin;
    float  width   = 0.0F;
    float  height  = 0.0F;
    bool   clicked = false;
};

/// The plate a row sits on. One step up from the panel at rest so that rows
/// read as rows on a page that is otherwise flat, and another step under the
/// pointer -- a plate the same color as what is behind it is not a plate.
constexpr ImU32 kCardRest  = IM_COL32(0x15, 0x15, 0x18, 0xFF);
constexpr ImU32 kCardHover = IM_COL32(0x1E, 0x1E, 0x24, 0xFF);

/// How far the text sits in from the edge of a card.
constexpr float kCardPadX = 0.8F;
constexpr float kCardPadY = 0.5F;

float card_height(int lines) {
    return em(kCardPadY) * 2.0F
         + ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(lines);
}

Card card_begin(const char* id, float height) {
    Card card;
    card.origin = ImGui::GetCursorScreenPos();
    card.width  = ImGui::GetContentRegionAvail().x;
    card.height = height;

    card.clicked = ImGui::InvisibleButton(id, ImVec2(card.width, height));

    ImGui::GetWindowDrawList()->AddRectFilled(
        card.origin, ImVec2(card.origin.x + card.width, card.origin.y + height),
        ImGui::IsItemHovered() ? kCardHover : kCardRest, em(0.45F));

    // Both halves of the inset. The cursor places the first line; the indent
    // is what puts the second and third under it, since ImGui returns to the
    // window's left margin after every item and would otherwise stack two
    // lines flush against the edge of the plate the first one is inset from.
    ImGui::Indent(em(kCardPadX));
    ImGui::SetCursorScreenPos(ImVec2(card.origin.x + em(kCardPadX),
                                     card.origin.y + em(kCardPadY)));
    return card;
}

/// A button drawn inside a card rather than placed over it.
///
/// A real ImGui button on top of the card's hit target loses the click to it:
/// SetNextItemAllowOverlap lets the later item win the hover, and the earlier
/// InvisibleButton still reported the press. Rather than fight that, the row
/// owns the one hit test it already has, and this is the affordance drawn
/// where it will land. Returns true when the pointer is over it, which is
/// what the caller checks against its own click.
bool card_action(const Card& card, const char* label, float from_top) {
    const ImVec2 min(card.origin.x + card.width - em(6.2F), card.origin.y + from_top);
    const ImVec2 max(min.x + em(5.2F), min.y + em(1.9F));
    const bool   over = ImGui::IsMouseHoveringRect(min, max);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, over ? theme::kFlame : theme::kPanelEdge, em(0.35F));
    const ImVec2 size = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(min.x + (max.x - min.x - size.x) * 0.5F,
                         min.y + (max.y - min.y - size.y) * 0.5F),
                  over ? theme::kInk : theme::kText, label);
    return over;
}

void card_end(const Card& card) {
    ImGui::Unindent(em(kCardPadX));
    ImGui::SetCursorScreenPos(ImVec2(card.origin.x, card.origin.y + card.height));
    ImGui::Dummy(ImVec2(0, em(0.4F)));
}

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
                note += "  \xc2\xb7  " + format::number(item.parameters_b, 1) + "B";
            }
            if (item.gated) {
                // Gated repositories need a browser and an accepted license.
                // Saying so here saves a download that would 401 an hour later.
                note += "  \xc2\xb7  gated: accept its license on Huggingface first";
            }
            text_colored(theme::kTextFaint, "%s", note.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    return picked;
}

// ---------------------------------------------------------------------------
// Saving
// ---------------------------------------------------------------------------

void App::lab_store(const lab::Recipe& recipe) {
    std::string error;
    if (!lab::save(recipe, error)) {
        lab_error_ = error;
        return;
    }
    lab_error_.clear();
    // Reread rather than patch the copy in hand. The list is what the disk
    // says, and a tab that kept its own idea of the list would eventually
    // disagree with the recipe it had just written.
    lab_saved_ = lab::saved_recipes();
    // Finishing a model changes what the expert picker can offer, and it is
    // the same question the models directory answers.
    refresh_models();
}

void App::start_training(const lab::Recipe& recipe) {
    lab::Recipe going = recipe;
    going.stage       = lab::Stage::Training;
    going.started_at  = lab::now_seconds();

    std::string error;
    if (!trainer_.start(going, convert_script(), quantize_bin(),
                        [this]() { glfwPostEmptyEvent(); }, error)) {
        // The stage is left alone. Marking something as training when
        // nothing is training is the lie this whole tab was rebuilt to stop
        // telling, and the reason it could not start is actionable -- almost
        // always that the Python environment is not installed.
        lab_error_ = error;
        say(error);
        return;
    }
    lab_run_applied_.clear();
    lab_store(going);
    lab_open_.clear();
    say(going.name + " is training");
}

// ---------------------------------------------------------------------------
// The tab
// ---------------------------------------------------------------------------

void App::draw_create(const Snapshot& snapshot) {
    // Read once: the disk is the list, and rereading it every frame to draw
    // four names would be four file reads at eleven frames a second.
    if (!lab_saved_read_) {
        lab_saved_      = lab::saved_recipes();
        lab_saved_read_ = true;
    }

    // A finished run, written back to the recipe it belongs to. Done here
    // rather than on the training page so that it happens whether or not
    // that page is the one being looked at -- a run that finishes while the
    // user is in Chat should still be ready to test when they come back.
    const lab::RunProgress run = trainer_.progress();
    if (run.phase == lab::RunProgress::Phase::Done && !run.recipe_id.empty()
        && run.recipe_id != lab_run_applied_) {
        lab_run_applied_ = run.recipe_id;
        for (const lab::Recipe& saved : lab_saved_) {
            if (saved.id != run.recipe_id) {
                continue;
            }
            lab::Recipe done   = saved;
            done.trained_path  = run.produced.string();
            done.stage         = lab::Stage::Testing;
            lab_store(done);
            say(done.name + " finished training -- it is ready to test");
        }
    }

    // By value, on purpose. Everything below writes recipes back through
    // lab_store(), which rereads the list -- a reference into it would dangle
    // the moment anything was saved.
    std::optional<lab::Recipe> open;
    if (!lab_open_.empty()) {
        for (const lab::Recipe& saved : lab_saved_) {
            if (saved.id == lab_open_) {
                open = saved;
            }
        }
        if (!open) {
            lab_open_.clear();  // deleted from under us
        }
    }

    if (open) {
        draw_create_detail(*open);
    } else {
        draw_create_list();
    }

    draw_create_wizard();
    draw_create_test(snapshot);
}

void App::draw_create_wizard_open(lab::Recipe recipe, int step) {
    lab_recipe_ = std::move(recipe);
    lab_step_   = std::clamp(step, 0, static_cast<int>(kSteps.size()) - 1);
    lab_query_.clear();
    lab_results_.clear();
    lab_local_path_.clear();
    lab_error_.clear();
    lab_wizard_want_ = true;
}



// ---------------------------------------------------------------------------
// The list
// ---------------------------------------------------------------------------

void App::draw_create_list() {
    ImGui::BeginChild("create-list", ImVec2(0, 0), ImGuiChildFlags_Borders);

    // Measured before the title is drawn, so the button lands against the
    // right edge of the row rather than of whatever is left after the title.
    const float row_left  = ImGui::GetCursorPosX();
    const float row_right = row_left + ImGui::GetContentRegionAvail().x;
    const float button    = em(8.0F);

    title("Experts");
    ImGui::SameLine(row_right - button);
    if (ImGui::Button("New expert", ImVec2(button, 0))) {
        draw_create_wizard_open(lab::Recipe{}, 0);
    }
    ImGui::SetItemTooltip("Walk through a fine-tune and start its run.");

    text_colored(theme::kTextFaint,
                 "Fine-tunes made here. One subject each, and the delegator picks "
                 "between them.");
    if (!lab_error_.empty()) {
        text_colored(theme::kError, "%s", lab_error_.c_str());
    }

    if (lab_saved_.empty()) {
        ImGui::Dummy(ImVec2(0, em(1.2F)));
        text_colored(theme::kText, "Nothing made yet.");
        wrapped(theme::kTextDim,
                "An expert is a small model taught one subject: a base model, the "
                "data to specialize it on, and a file at the end that any llama.cpp "
                "program can load. New expert walks through it.");
        ImGui::EndChild();
        return;
    }

    // A copy for the frame. A row can delete or advance the recipe it is
    // drawing, which rereads lab_saved_ underneath this loop.
    const std::vector<lab::Recipe> rows = lab_saved_;

    const auto draw_row = [&](const lab::Recipe& recipe) {
        ImGui::PushID(recipe.id.c_str());
        const Card card = card_begin("##row", card_height(3));

        const float text_room = card.width - em(9.6F);
        text_colored(theme::kText, "%s",
                     elide(recipe.name.empty() ? "(unnamed)" : recipe.name, text_room).c_str());
        ImGui::SameLine();
        text_colored(stage_color(recipe.stage), "%s", lab::stage_text(recipe.stage));

        const std::string purpose =
            recipe.purpose.empty() ? std::string("no description") : recipe.purpose;
        text_colored(recipe.purpose.empty() ? theme::kTextFaint : theme::kTextDim, "%s",
                     elide(purpose, text_room).c_str());
        text_colored(theme::kTextFaint, "%s", elide(spec_line(recipe), text_room).c_str());

        // The one action worth reaching without opening the expert first. The
        // rest live on its page, where there is room to say what they do.
        const bool testable = recipe.stage == lab::Stage::Testing
                           || recipe.stage == lab::Stage::Finished;
        const bool on_test  = testable && card_action(card, "Test", em(0.9F));

        card_end(card);
        if (card.clicked) {
            lab_open_ = recipe.id;
            lab_error_.clear();
            if (on_test && file_bytes(recipe.trained_path) > 0) {
                lab_test_      = recipe.id;
                lab_test_want_ = true;
            }
        }
        ImGui::PopID();
    };

    const auto group = [&](const char* heading, const char* note, lab::Stage stage) {
        std::size_t count = 0;
        for (const lab::Recipe& recipe : rows) {
            count += recipe.stage == stage ? 1 : 0;
        }
        if (count == 0) {
            return;
        }
        section(heading);
        if (note != nullptr) {
            text_colored(theme::kTextFaint, "%s", note);
        }
        ImGui::Dummy(ImVec2(0, em(0.2F)));
        for (const lab::Recipe& recipe : rows) {
            if (recipe.stage == stage) {
                draw_row(recipe);
            }
        }
    };

    // Finished first: what has been made is what the tab is a list of. The
    // ones still happening come next because they are the ones with a next
    // step, and drafts last because a draft is a note to self.
    group("FINISHED", "Tested and kept. These are offered to every seat.",
          lab::Stage::Finished);
    group("READY TO TEST", "Trained. Talk to one before you keep it.", lab::Stage::Testing);
    group("TRAINING", "Started. Open one for its progress.", lab::Stage::Training);
    group("DRAFTS", "Not started.", lab::Stage::Draft);

    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// One expert
// ---------------------------------------------------------------------------

void App::draw_create_detail(lab::Recipe recipe) {
    ImGui::BeginChild("create-detail", ImVec2(0, 0), ImGuiChildFlags_Borders);

    if (ImGui::Button("All experts", ImVec2(em(8.0F), 0))) {
        lab_open_.clear();
        // Whatever went wrong went wrong on this page. Carrying it to the
        // list would report it against a recipe it has nothing to do with.
        lab_error_.clear();
    }
    ImGui::Dummy(ImVec2(0, em(0.4F)));

    title(recipe.name.empty() ? "(unnamed)" : recipe.name.c_str());
    ImGui::SameLine();
    text_colored(stage_color(recipe.stage), "%s", lab::stage_text(recipe.stage));
    if (!recipe.purpose.empty()) {
        wrapped(theme::kTextDim, recipe.purpose);
    }
    if (!lab_error_.empty()) {
        text_colored(theme::kError, "%s", lab_error_.c_str());
    }

    // A label column and a value column. Two ems wider than the longest label,
    // so the values line up and the page can be read down rather than across.
    const auto spec = [](const char* label, const std::string& value,
                         ImU32 color = theme::kText) {
        if (value.empty()) {
            return;
        }
        text_colored(theme::kTextFaint, "%s", label);
        ImGui::SameLine(em(9.0F));
        text_colored(color, "%s", value.c_str());
    };

    section("SPECS");
    spec("Base model", recipe.base.chosen()
                           ? (recipe.base.label.empty() ? recipe.base.id : recipe.base.label)
                           : std::string("not chosen"),
         recipe.base.chosen() ? theme::kText : theme::kFlameBright);
    if (recipe.parameters_b > 0.0) {
        spec("Size", format::number(recipe.parameters_b, 1) + " billion parameters");
    }
    spec("Method", method_text(recipe.method)
                       + (recipe.method == lab::Method::Lora
                              ? " -- an adapter over the base in sixteen bits"
                              : " -- an adapter over a four-bit base"));

    if (recipe.data.empty()) {
        spec("Data", "none", theme::kFlameBright);
    } else {
        std::string data = recipe.data.front().id;
        for (std::size_t i = 1; i < recipe.data.size(); ++i) {
            data += ", " + recipe.data[i].id;
        }
        spec("Data", data);
    }
    if (!recipe.tools.empty()) {
        std::string tools = recipe.tools.front().id;
        for (std::size_t i = 1; i < recipe.tools.size(); ++i) {
            tools += ", " + recipe.tools[i].id;
        }
        spec("Tools", tools);
    }

    spec("Run", std::to_string(recipe.epochs)
                    + (recipe.epochs == 1 ? " pass" : " passes")
                    + "  \xc2\xb7  " + std::to_string(recipe.context) + " tokens of context"
                    + "  \xc2\xb7  learning rate " + rate_text(recipe.learning_rate));

    const std::uint64_t exported = lab::export_bytes(recipe.parameters_b, recipe.quantization);
    spec("Export", std::string(recipe.format == lab::Export::Mlx ? "MLX" : "GGUF") + " at "
                       + recipe.quantization
                       + (exported > 0 ? ", about " + format::bytes(exported) : ""));

    const std::uint64_t vram = vram_here();
    if (recipe.parameters_b > 0.0) {
        const lab::Fit fit = lab::estimate_fit(recipe.parameters_b, recipe.method, vram, 0);
        if (vram == 0) {
            spec("Fits", "no GPU runtime installed, so nothing can say", theme::kTextDim);
        } else {
            spec("Fits", "needs about " + format::bytes(fit.needed) + " of the "
                             + format::bytes(fit.have) + " this machine has",
                 fit.possible ? theme::kText : theme::kFlameBright);
        }
    }

    const std::uint64_t trained = file_bytes(recipe.trained_path);
    if (!recipe.trained_path.empty()) {
        // Cut from the middle, and the size kept whole. A path is identified
        // at both ends, and running it off the right edge of the window would
        // take the one number on the line with it.
        const std::string size = trained > 0 ? "  (" + format::bytes(trained) + ")"
                                             : "  (not there)";
        const float       room = ImGui::GetContentRegionAvail().x - em(9.6F)
                               - ImGui::CalcTextSize(size.c_str()).x;
        spec("File", middle_out(recipe.trained_path, room) + size,
             trained > 0 ? theme::kText : theme::kError);
    }
    spec("Started", when_text(recipe.started_at));
    spec("Finished", when_text(recipe.finished_at));

    // --- what can be done about it, which is the whole reason for the page --
    const std::vector<std::string> gaps = lab::missing(recipe);

    switch (recipe.stage) {
        case lab::Stage::Draft: {
            section("BEFORE IT CAN RUN");
            if (gaps.empty()) {
                text_colored(theme::kText, "Nothing missing. It can be started.");
            } else {
                for (const std::string& gap : gaps) {
                    text_colored(theme::kFlameBright, "still needs %s", gap.c_str());
                }
            }
            // Said before the button rather than after it is pressed. The
            // environment is the one prerequisite somebody cannot guess at,
            // and the fix is a page away.
            const bool trainable = lab::pyenv::looks_installed();
            if (!trainable) {
                ImGui::Dummy(ImVec2(0, em(0.4F)));
                wrapped(theme::kFlameBright,
                        "The fine-tuner is not set up on this machine.");
                text_colored(theme::kTextFaint,
                             "Settings, Training installs it -- a private Python, a "
                             "few gigabytes, once.");
            }

            ImGui::Dummy(ImVec2(0, em(0.6F)));
            if (ImGui::Button("Continue setup", ImVec2(em(9.0F), 0))) {
                draw_create_wizard_open(recipe, 0);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!gaps.empty() || !trainable);
            if (ImGui::Button("Start training", ImVec2(em(9.0F), 0))) {
                start_training(recipe);
            }
            ImGui::EndDisabled();
            if (!trainable) {
                ImGui::SameLine();
                if (ImGui::Button("Set up training", ImVec2(em(11.0F), 0))) {
                    show_settings(SettingsPage::Training);
                }
            }
            break;
        }

        case lab::Stage::Training: {
            section("PROGRESS");
            const lab::RunProgress run  = trainer_.progress();
            const bool             mine = run.recipe_id == recipe.id
                                       && run.phase != lab::RunProgress::Phase::Idle;

            if (mine && run.running()) {
                text_colored(theme::kText, "%s", run.label().c_str());
                if (run.phase == lab::RunProgress::Phase::Training && run.step_total > 0) {
                    ImGui::ProgressBar(run.percent(), ImVec2(em(26.0F), em(0.8F)), "");
                    const long long left = run.seconds_left();
                    if (left > 0) {
                        text_colored(theme::kTextFaint, "about %s left",
                                     left < 90 ? "a minute"
                                               : (std::to_string(left / 60) + " minutes").c_str());
                    }
                } else {
                    // Everything before the first optimizer step: the model
                    // coming off the disk, the tokenizer, the data. No bar,
                    // because there is no denominator -- and an indeterminate
                    // bar that sweeps for four minutes says less than a word.
                    text_colored(theme::kTextFaint, "running for %s",
                                 since_text(recipe.started_at).c_str());
                }

                if (!run.device.empty()) {
                    spec("On", run.device);
                }
                if (run.records > 0) {
                    spec("Data", std::to_string(run.records) + " records");
                }
                if (run.trainable > 0) {
                    spec("Adapter", format::number(
                             static_cast<double>(run.trainable) / 1e6, 1) + "M parameters");
                }

                // The loss curve, because it is the one number that says
                // whether a run is working rather than merely progressing. A
                // line that has been flat for three hundred steps is a run
                // worth stopping.
                if (run.curve.size() > 2) {
                    ImGui::PlotLines("##loss", run.curve.data(),
                                     static_cast<int>(run.curve.size()), 0, "loss",
                                     FLT_MAX, FLT_MAX, ImVec2(em(26.0F), em(4.0F)));
                }
                for (const std::string& note : run.notes) {
                    wrapped(theme::kTextDim, note);
                }

                ImGui::Dummy(ImVec2(0, em(0.5F)));
                if (ImGui::Button("Stop the run", ImVec2(em(10.0F), 0))) {
                    trainer_.cancel();
                }
                ImGui::SetItemTooltip("Stops within a step. What it has learned so "
                                      "far is not kept.");
                break;
            }

            if (mine && run.phase == lab::RunProgress::Phase::Failed) {
                wrapped(theme::kError, run.error);
                if (!run.hint.empty()) {
                    wrapped(theme::kTextDim, run.hint);
                }
                for (const std::string& entry : run.log_tail) {
                    text_colored(theme::kTextFaint, "%s",
                                 elide(entry, ImGui::GetContentRegionAvail().x).c_str());
                }
                if (!run.log_file.empty()) {
                    text_colored(theme::kTextFaint, "full log: %s",
                                 middle_out(run.log_file.string(),
                                            ImGui::GetContentRegionAvail().x - em(9.0F))
                                     .c_str());
                }
                ImGui::Dummy(ImVec2(0, em(0.5F)));
                if (ImGui::Button("Try again", ImVec2(em(9.0F), 0))) {
                    trainer_.dismiss();
                    start_training(recipe);
                }
                ImGui::SameLine();
                if (ImGui::Button("Edit the recipe", ImVec2(em(11.0F), 0))) {
                    trainer_.dismiss();
                    draw_create_wizard_open(recipe, kStepTarget);
                }
                break;
            }

            if (mine && run.phase == lab::RunProgress::Phase::Canceled) {
                text_colored(theme::kTextDim, "Stopped. Nothing was kept.");
                ImGui::Dummy(ImVec2(0, em(0.5F)));
                if (ImGui::Button("Start again", ImVec2(em(9.0F), 0))) {
                    trainer_.dismiss();
                    start_training(recipe);
                }
                break;
            }

            // Marked as training with nothing running. A run is a child of
            // this process, so closing Crucible ends it -- and the recipe
            // outlives the process that was training it.
            if (recipe.started_at > 0) {
                text_colored(theme::kText, "Started %s ago.",
                             since_text(recipe.started_at).c_str());
            }
            wrapped(theme::kFlameBright, "Nothing is running.");
            wrapped(theme::kTextDim,
                    "A run belongs to the window that started it, so closing Crucible "
                    "stops one. Starting again begins from the base model: there is no "
                    "checkpoint to resume from yet.");
            ImGui::Dummy(ImVec2(0, em(0.5F)));
            if (ImGui::Button("Start again", ImVec2(em(9.0F), 0))) {
                start_training(recipe);
            }
            ImGui::SameLine();
            if (ImGui::Button("Put it back to a draft##restart", ImVec2(em(12.0F), 0))) {
                lab::Recipe back = recipe;
                back.stage       = lab::Stage::Draft;
                back.started_at  = 0;
                lab_store(back);
            }

            section("A FILE TRAINED SOMEWHERE ELSE");
            wrapped(theme::kTextDim,
                    "Trained this recipe with unsloth, axolotl or mlx_lm? Point at "
                    "what came out and the rest works today: test it, keep it, and "
                    "it takes a seat on the roster.");
            ImGui::SetNextItemWidth(em(30.0F));
            ImGui::InputTextWithHint("##lab-attach", "/path/to/expert.gguf", &lab_attach_);
            ImGui::SameLine();
            ImGui::BeginDisabled(lab_attach_.empty());
            if (ImGui::Button("Attach")) {
                std::error_code ec;
                const std::filesystem::path file(lab_attach_);
                if (!std::filesystem::is_regular_file(file, ec)) {
                    lab_error_ = lab_attach_ + " is not a file on this machine";
                } else {
                    recipe.trained_path = std::filesystem::absolute(file, ec).string();
                    if (ec) {
                        recipe.trained_path = lab_attach_;
                    }
                    recipe.stage = lab::Stage::Testing;
                    lab_attach_.clear();
                    lab_store(recipe);
                    say(recipe.name + " is ready to test");
                }
            }
            ImGui::EndDisabled();

            break;
        }

        case lab::Stage::Testing: {
            section("BEFORE YOU KEEP IT");
            wrapped(theme::kTextDim,
                    "Talk to it. Finishing puts it on the roster where the delegator "
                    "can route to it; editing goes back to the fine-tune parameters.");
            ImGui::Dummy(ImVec2(0, em(0.5F)));
            ImGui::BeginDisabled(trained == 0);
            if (ImGui::Button("Test", ImVec2(em(7.0F), 0))) {
                lab_test_      = recipe.id;
                lab_test_want_ = true;
            }
            ImGui::EndDisabled();
            if (trained == 0) {
                ImGui::SetItemTooltip("The file this recipe points at is not there.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Edit", ImVec2(em(7.0F), 0))) {
                draw_create_wizard_open(recipe, kStepTarget);
            }
            break;
        }

        case lab::Stage::Finished: {
            section("ON THE ROSTER");
            wrapped(theme::kTextDim,
                    "Every seat can be pointed at this model from Settings, Experts.");
            ImGui::Dummy(ImVec2(0, em(0.5F)));
            ImGui::BeginDisabled(trained == 0);
            if (ImGui::Button("Test again", ImVec2(em(8.0F), 0))) {
                lab_test_      = recipe.id;
                lab_test_want_ = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Edit", ImVec2(em(7.0F), 0))) {
                draw_create_wizard_open(recipe, kStepTarget);
            }
            break;
        }
    }

    // --- removal, behind a question ----------------------------------------
    ImGui::Dummy(ImVec2(0, em(1.0F)));
    if (ImGui::Button("Delete this expert", ImVec2(em(11.0F), 0))) {
        ImGui::OpenPopup("Delete this expert?");
    }
    ImGui::SetNextWindowSize(ImVec2(em(28.0F), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Delete this expert?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        wrapped(theme::kTextDim,
                "The recipe and its folder go. A model file it exported is left "
                "exactly where it is -- deleting a recipe should not delete "
                "gigabytes somebody spent an evening making.");
        ImGui::Dummy(ImVec2(0, em(0.5F)));
        if (ImGui::Button("Delete", ImVec2(em(7.0F), 0))) {
            std::error_code ec;
            std::filesystem::remove_all(lab::recipe_file(recipe).parent_path(), ec);
            if (ec) {
                lab_error_ = "could not remove the recipe folder: " + ec.message();
            } else {
                say(recipe.name + " was deleted");
            }
            lab_open_.clear();
            lab_saved_ = lab::saved_recipes();
            refresh_models();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep", ImVec2(em(7.0F), 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// The wizard
// ---------------------------------------------------------------------------

void App::draw_create_wizard() {
    // The id after ### is fixed so that OpenPopup and BeginPopupModal agree
    // across frames; the part before it is what the window is called, and
    // "Create an expert" over a recipe that already exists is a lie about
    // what pressing Save will do.
    const std::string heading = (lab_recipe_.id.empty() ? std::string("Create an expert")
                                                        : "Edit " + lab_recipe_.name)
                              + "###create-wizard";
    const char* const kTitle = heading.c_str();
    if (lab_wizard_want_) {
        ImGui::OpenPopup(kTitle);
        lab_wizard_want_ = false;
        lab_wizard_up_   = true;
    }
    if (!lab_wizard_up_) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(em(46.0F), em(36.0F)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        lab_wizard_up_ = false;
        return;
    }

    const auto save_now = [this]() {
        std::string error;
        if (!lab_recipe_.name.empty() && !lab::save(lab_recipe_, error)) {
            lab_error_ = error;
        }
    };
    const auto leave_step = [this]() {
        // Each step searches for something else. Models left in the box when
        // the data step opens are not results, they are litter.
        lab_query_.clear();
        lab_results_.clear();
        lab_error_.clear();
        lab_local_path_.clear();
    };

    const int step = std::clamp(lab_step_, 0, static_cast<int>(kSteps.size()) - 1);

    // --- where you are, across the top -------------------------------------
    //
    // Chips rather than a rail down the side: a modal is wider than it is
    // deep, and six words fit across the top of one without taking a column
    // away from the thing being filled in.
    for (std::size_t i = 0; i < kSteps.size(); ++i) {
        if (i > 0) {
            ImGui::SameLine();
            text_colored(theme::kTextFaint, "\xc2\xb7");
            ImGui::SameLine();
        }
        const bool here = static_cast<int>(i) == step;
        const bool done = (i == 0 && !lab_recipe_.name.empty())
                       || (i == 1 && lab_recipe_.base.chosen())
                       || (i == 2 && !lab_recipe_.data.empty())
                       || (i == 3 && !lab_recipe_.tools.empty())
                       || (i == 4 && lab_recipe_.parameters_b > 0.0);
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              here ? theme::kFlame : (done ? theme::kText : theme::kTextFaint));
        if (ImGui::SmallButton(kSteps[i].label)) {
            save_now();
            leave_step();
            lab_step_ = static_cast<int>(i);
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    ImGui::Dummy(ImVec2(0, em(0.3F)));
    title(kSteps[static_cast<std::size_t>(step)].label);
    text_colored(theme::kTextFaint, "%s", kSteps[static_cast<std::size_t>(step)].blurb);
    if (!lab_error_.empty() && step != 1 && step != 2 && step != 3) {
        // The picker prints its own; everywhere else this is the only place a
        // failed save would otherwise be seen, which is to say not at all.
        text_colored(theme::kError, "%s", lab_error_.c_str());
    }

    // The body scrolls and the footer does not, so Back and Next are in the
    // same place on every step whatever is above them.
    ImGui::Dummy(ImVec2(0, em(0.3F)));
    ImGui::BeginChild("wizard-body", ImVec2(0, -em(2.8F)));
    draw_create_step(lab_recipe_, step);
    ImGui::EndChild();

    // --- the footer ---------------------------------------------------------
    const std::vector<std::string> gaps = lab::missing(lab_recipe_);
    const float                    row_right = ImGui::GetCursorPosX()
                                             + ImGui::GetContentRegionAvail().x;

    // A recipe with nothing to name it has no directory to live in, so
    // leaving is leaving rather than saving, and the button says which.
    const bool nameable = !lab::slug_of(lab_recipe_.name).empty();
    if (ImGui::Button(nameable ? "Save and close" : "Discard", ImVec2(em(9.0F), 0))) {
        if (nameable) {
            save_now();
            lab_store(lab_recipe_);
        }
        lab_error_.clear();
        lab_wizard_up_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SetItemTooltip(nameable ? "Kept as a draft. Nothing is lost by leaving."
                                   : "Nothing has been named, so there is nothing to keep.");

    const float wide = em(9.5F);
    if (step == kStepReview) {
        ImGui::SameLine(row_right - wide - em(6.0F) - ImGui::GetStyle().ItemSpacing.x);
        ImGui::BeginDisabled(step == 0);
        if (ImGui::Button("Back", ImVec2(em(6.0F), 0))) {
            save_now();
            leave_step();
            --lab_step_;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!gaps.empty());
        if (ImGui::Button("Start training", ImVec2(wide, 0))) {
            save_now();
            // Back to the list, which is where the model now is and where
            // its progress is clicked through to.
            start_training(lab_recipe_);
            lab_wizard_up_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        if (!gaps.empty()) {
            ImGui::SetItemTooltip("Still needs %s.", gaps.front().c_str());
        }
    } else {
        ImGui::SameLine(row_right - em(6.0F) * 2.0F - ImGui::GetStyle().ItemSpacing.x);
        ImGui::BeginDisabled(step == 0);
        if (ImGui::Button("Back", ImVec2(em(6.0F), 0))) {
            save_now();
            leave_step();
            --lab_step_;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Next", ImVec2(em(6.0F), 0))) {
            save_now();
            leave_step();
            ++lab_step_;
        }
    }

    ImGui::EndPopup();
}

void App::draw_create_step(lab::Recipe& recipe, int step) {
    const auto save_now = [this, &recipe]() {
        std::string error;
        if (!recipe.name.empty() && !lab::save(recipe, error)) {
            lab_error_ = error;
        }
    };

    switch (step) {
        case 0: {
            // No "NAME" heading over the first box: the step is called Name
            // and its title is two lines above. The description gets one
            // because it is the second field, not because it needs saying.
            ImGui::Dummy(ImVec2(0, em(0.2F)));
            ImGui::SetNextItemWidth(em(22.0F));
            if (ImGui::InputTextWithHint("##lab-name", "Kitchen Physicist", &recipe.name)) {
                recipe.id = lab::slug_of(recipe.name);
            }
            if (!recipe.id.empty()) {
                text_colored(theme::kTextFaint, "saved as %s", recipe.id.c_str());
            }
            section("DESCRIPTION");
            ImGui::SetNextItemWidth(em(30.0F));
            ImGui::InputTextWithHint("##lab-purpose", "answers questions about induction hobs",
                                     &recipe.purpose);
            // Not optional in the way it used to say. This is the field the
            // delegator routes on, and a seat with only a name to go on is one
            // it cannot tell from its neighbors.
            text_colored(theme::kTextFaint,
                         "What the delegator routes on. Name the things it should take.");
            break;
        }

        case 1: {
            section("FROM HUGGINGFACE");
            if (const std::optional<lab::Asset> picked =
                    draw_hub_picker(lab::hub::Kind::Model, "llama 3.2 1b, qwen 0.5b, smollm...")) {
                recipe.base         = *picked;
                recipe.parameters_b = lab::hub::parameters_from_name(picked->id);
                for (const lab::hub::Item& item : lab_results_) {
                    if (item.id == picked->id && item.parameters_b > 0.0) {
                        recipe.parameters_b = item.parameters_b;
                    }
                }
                save_now();
            }

            section("OR A FILE ON THIS MACHINE");
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
                recipe.base  = asset;
                if (recipe.parameters_b <= 0.0) {
                    recipe.parameters_b = lab::hub::parameters_from_name(asset.label);
                }
                lab_local_path_.clear();
                save_now();
            }
            ImGui::EndDisabled();

            if (recipe.base.chosen()) {
                ImGui::Dummy(ImVec2(0, em(0.5F)));
                section("CHOSEN");
                text_colored(theme::kText, "%s", recipe.base.id.c_str());
                if (recipe.parameters_b > 0.0) {
                    text_colored(theme::kTextFaint, "%s billion parameters",
                                 format::number(recipe.parameters_b, 1).c_str());
                }
                const std::uint64_t vram = vram_here();
                const lab::Fit fit = lab::estimate_fit(recipe.parameters_b, recipe.method,
                                                       vram, 0);
                if (vram == 0) {
                    // An empty device list means no GPU runtime is installed,
                    // which is not the same as no GPU: the cards are there and
                    // nothing can see them yet.
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
            const bool tools = step == 3;
            std::vector<lab::Asset>& into = tools ? recipe.tools : recipe.data;
            if (tools) {
                text_colored(theme::kTextFaint, "Transcripts of a model using tools.");
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
            text_colored(theme::kTextFaint, "A file or a folder: text, JSONL, markdown.");
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

        case kStepTarget: {
            section("METHOD");
            const std::uint64_t vram = vram_here();
            // No sentence under each one saying what an adapter is. The number
            // beside it -- what this run needs against what the card has -- is
            // the thing that decides between them, and it was being read past.
            const auto method_row = [&](const char* label, lab::Method method) {
                const bool on = recipe.method == method;
                if (ImGui::RadioButton(label, on)) {
                    recipe.method = method;
                    save_now();
                }
                if (recipe.parameters_b > 0.0 && vram > 0) {
                    const lab::Fit fit = lab::estimate_fit(recipe.parameters_b, method, vram, 0);
                    ImGui::SameLine();
                    text_colored(fit.possible ? theme::kText : theme::kFlameBright,
                                 "needs about %s, this machine has %s",
                                 format::bytes(fit.needed).c_str(),
                                 format::bytes(fit.have).c_str());
                }
            };
            method_row("QLoRA", lab::Method::Qlora);
            method_row("LoRA", lab::Method::Lora);

            section("SIZE");
            for (const char* quant : kQuantizations) {
                const bool on = recipe.quantization == quant;
                ImGui::PushID(quant);
                if (ImGui::RadioButton(quant, on)) {
                    recipe.quantization = quant;
                    save_now();
                }
                if (recipe.parameters_b > 0.0) {
                    ImGui::SameLine();
                    text_colored(theme::kTextFaint, "about %s",
                                 format::bytes(lab::export_bytes(recipe.parameters_b, quant))
                                     .c_str());
                }
                ImGui::PopID();
            }

            section("THE RUN");
            ImGui::SetNextItemWidth(em(14.0F));
            if (ImGui::SliderInt("Passes over the data", &recipe.epochs, 1, 10)) {
                save_now();
            }
            ImGui::SetNextItemWidth(em(14.0F));
            if (ImGui::SliderInt("Context", &recipe.context, 256, 4096)) {
                save_now();
            }
            ImGui::SetNextItemWidth(em(14.0F));
            if (ImGui::SliderFloat("Learning rate", &recipe.learning_rate, 1e-6F, 1e-3F,
                                   "%.1e", ImGuiSliderFlags_Logarithmic)) {
                save_now();
            }

            section("FORMAT");
            if (ImGui::RadioButton("GGUF", recipe.format == lab::Export::Gguf)) {
                recipe.format = lab::Export::Gguf;
                save_now();
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("MLX", recipe.format == lab::Export::Mlx)) {
                recipe.format = lab::Export::Mlx;
                save_now();
            }
            break;
        }

        default: {
            // Review. Everything chosen, read back in one page, because the
            // step before starting something that takes an evening is the one
            // place a wrong base model is cheap to notice.
            const auto line = [](const char* label, const std::string& value, ImU32 color) {
                text_colored(theme::kTextFaint, "%s", label);
                ImGui::SameLine(em(9.0F));
                text_colored(color, "%s", value.c_str());
            };

            section("WHAT WILL RUN");
            line("Name", recipe.name.empty() ? "(unnamed)" : recipe.name,
                 recipe.name.empty() ? theme::kFlameBright : theme::kText);
            line("For", recipe.purpose.empty() ? "no description" : recipe.purpose,
                 recipe.purpose.empty() ? theme::kFlameBright : theme::kTextDim);
            line("Base model",
                 recipe.base.chosen() ? recipe.base.id : std::string("not chosen"),
                 recipe.base.chosen() ? theme::kText : theme::kFlameBright);
            line("Data", recipe.data.empty()
                             ? std::string("none")
                             : std::to_string(recipe.data.size())
                                   + (recipe.data.size() == 1 ? " set" : " sets"),
                 recipe.data.empty() ? theme::kFlameBright : theme::kText);
            line("Tools",
                 recipe.tools.empty() ? std::string("none")
                                      : std::to_string(recipe.tools.size()) + " sets",
                 theme::kText);
            line("Method", method_text(recipe.method), theme::kText);
            line("Run", std::to_string(recipe.epochs)
                            + (recipe.epochs == 1 ? " pass" : " passes") + "  \xc2\xb7  "
                            + std::to_string(recipe.context) + " tokens of context",
                 theme::kText);
            const std::uint64_t exported =
                lab::export_bytes(recipe.parameters_b, recipe.quantization);
            line("Export",
                 std::string(recipe.format == lab::Export::Mlx ? "MLX" : "GGUF") + " at "
                     + recipe.quantization
                     + (exported > 0 ? ", about " + format::bytes(exported) : ""),
                 theme::kText);

            const std::uint64_t vram = vram_here();
            if (recipe.parameters_b > 0.0 && vram > 0) {
                const lab::Fit fit =
                    lab::estimate_fit(recipe.parameters_b, recipe.method, vram, 0);
                line("Fits", "needs about " + format::bytes(fit.needed) + " of "
                                 + format::bytes(fit.have),
                     fit.possible ? theme::kText : theme::kFlameBright);
            }

            const std::vector<std::string> gaps = lab::missing(recipe);
            if (!gaps.empty()) {
                section("STILL MISSING");
                for (const std::string& gap : gaps) {
                    text_colored(theme::kFlameBright, "%s", gap.c_str());
                }
            }

            section("WHAT STARTING IT DOES TODAY");
            wrapped(theme::kTextDim,
                    "Puts it in the list as training and closes this. Crucible does "
                    "not run the fine-tune itself yet -- the trainer wants a Python "
                    "environment it does not install -- so nothing is spent and "
                    "nothing is downloaded. Its page is where a run will report "
                    "itself, and where a model trained elsewhere is attached.");
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// The test window
// ---------------------------------------------------------------------------

void App::lab_seat_test(const lab::Recipe& recipe) {
    Config edited = config_;

    Expert expert;
    expert.id   = kTestSeat;
    expert.name = recipe.name + " (testing)";
    expert.blurb = recipe.purpose.empty()
                       ? std::string("A fine-tune being tested before it is kept.")
                       : recipe.purpose;

    std::string error;
    if (!edited.roster.add(std::move(expert), error)) {
        lab_error_ = error;
        return;
    }
    ModelParams params;
    params.model            = recipe.trained_path;
    edited.experts[kTestSeat] = params;

    // Handed to the engine and nowhere else. update_config() would write this
    // to the config file, and a seat called "(testing)" surviving a crash is
    // exactly the litter this avoids.
    engine_->apply_config(edited);
}

void App::lab_unseat_test() {
    // The seat only ever existed in the copy the engine was given, so handing
    // back the real configuration is the whole of the undo.
    engine_->apply_config(config_);
}

void App::draw_create_test(const Snapshot& snapshot) {
    const lab::Recipe* found = nullptr;
    for (const lab::Recipe& saved : lab_saved_) {
        if (!lab_test_.empty() && saved.id == lab_test_) {
            found = &saved;
        }
    }
    if (found == nullptr) {
        lab_test_want_ = false;
        lab_test_up_   = false;
        return;
    }
    const lab::Recipe recipe = *found;

    const std::string label = "Test " + recipe.name + "###create-test";
    if (lab_test_want_) {
        ImGui::OpenPopup(label.c_str());
        lab_test_want_ = false;
        lab_test_up_   = true;
        // Where this conversation starts. Everything before it belongs to the
        // project and is not this window's business.
        lab_test_from_ = snapshot.turns.size();
        lab_test_prompt_.clear();
        lab_seat_test(recipe);
    }
    if (!lab_test_up_) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(em(54.0F), em(38.0F)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(label.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        lab_test_up_ = false;
        return;
    }

    text_colored(theme::kTextFaint, "%s",
                 middle_out(recipe.trained_path, ImGui::GetContentRegionAvail().x).c_str());
    wrapped(theme::kTextFaint,
            "The same engine as Chat, with this file seated for as long as this window "
            "is open. What you ask it appears in the transcript too.");
    if (!lab_error_.empty()) {
        text_colored(theme::kError, "%s", lab_error_.c_str());
    }

    // --- the conversation ---------------------------------------------------
    const float footer = em(3.0F) + grow_input_height(lab_test_prompt_,
                                                      ImGui::GetContentRegionAvail().x
                                                          - em(6.0F),
                                                      4);
    ImGui::BeginChild("test-talk", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    if (lab_test_from_ >= snapshot.turns.size()) {
        text_colored(theme::kTextDim, "Ask it something it should be good at.");
    }
    for (std::size_t i = lab_test_from_; i < snapshot.turns.size(); ++i) {
        const Turn& turn = snapshot.turns[i];
        ImGui::PushID(static_cast<int>(i));
        text_colored(theme::kFlame, "you");
        wrapped(theme::kText, turn.prompt);
        ImGui::Dummy(ImVec2(0, em(0.3F)));
        text_colored(theme::kFlameBright, "%s", recipe.name.c_str());
        if (turn.reply.empty() && turn.streaming) {
            text_colored(theme::kTextDim, "thinking...");
        } else if (turn.failed) {
            wrapped(theme::kError, turn.reply.empty() ? "it could not answer" : turn.reply);
        } else {
            // Through the same renderer the transcript uses. A candidate that
            // writes a fenced code block should be judged on the code, not on
            // three backticks -- and how it formats an answer is part of what
            // is being decided here.
            draw_markdown(turn.reply, theme::kText);
        }
        ImGui::Dummy(ImVec2(0, em(0.5F)));
        ImGui::PopID();
    }
    // Follow the answer down while it arrives, as the chat transcript does.
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - em(2.0F)) {
        ImGui::SetScrollHereY(1.0F);
    }
    ImGui::EndChild();

    // --- the composer -------------------------------------------------------
    const float send = em(5.4F);
    const float room = ImGui::GetContentRegionAvail().x - send - ImGui::GetStyle().ItemSpacing.x;
    const bool  busy = snapshot.busy;
    ImGui::BeginDisabled(busy);
    bool submit = grow_input("##test-prompt", "Ask it something", lab_test_prompt_, room, 4);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || lab_test_prompt_.empty());
    submit = ImGui::Button("Send", ImVec2(send, 0)) || submit;
    ImGui::EndDisabled();
    if (submit && !busy && !lab_test_prompt_.empty()) {
        engine_->submit(lab_test_prompt_, ExpertId(kTestSeat));
        lab_test_prompt_.clear();
    }

    // --- and what you decided ----------------------------------------------
    ImGui::Dummy(ImVec2(0, em(0.2F)));
    if (ImGui::Button("Finish", ImVec2(em(7.0F), 0))) {
        lab_unseat_test();

        lab::Recipe kept   = recipe;
        kept.stage         = lab::Stage::Finished;
        kept.finished_at   = lab::now_seconds();
        lab_store(kept);

        // And a seat, which is what finishing is for. Skipped where the roster
        // already has one by that name: the second Finish on the same expert
        // should not make a duplicate of it.
        Config edited = config_;
        if (edited.roster.find(kept.name)) {
            say(kept.name + " is finished");
        } else {
            Expert expert;
            expert.name  = kept.name;
            expert.blurb = kept.purpose;
            std::string error;
            if (!edited.roster.add(expert, error)) {
                say(error);
            } else {
                const ExpertId id = make_expert_id(kept.name);
                ModelParams    params;
                params.model       = kept.trained_path;
                edited.experts[id] = params;
                update_config([&edited](Config& config) { config = edited; });
                engine_->write_examples(id);
                say(kept.name + " is finished and has joined the experts");
            }
        }

        lab_open_    = kept.id;
        lab_test_.clear();
        lab_test_up_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SetItemTooltip("Keeps it, and gives it a seat the delegator can route to.");

    ImGui::SameLine();
    if (ImGui::Button("Edit", ImVec2(em(7.0F), 0))) {
        lab_unseat_test();
        lab_open_ = recipe.id;
        draw_create_wizard_open(recipe, kStepTarget);
        lab_test_.clear();
        lab_test_up_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SetItemTooltip("Back to the fine-tune parameters.");

    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(em(7.0F), 0))) {
        lab_unseat_test();
        lab_test_.clear();
        lab_test_up_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    text_colored(theme::kTextFaint, "Finishing puts it on the roster. Closing changes nothing.");

    ImGui::EndPopup();
}

}  // namespace crucible::gui
