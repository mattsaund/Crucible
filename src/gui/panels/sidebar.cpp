// SPDX-License-Identifier: MIT
//
// The side menu: the delegator, the experts under it, and the line joining the
// two while a turn is flowing.
//
// It is one picture of one thing -- how a prompt gets answered -- rather than a
// list of unrelated sections. The delegator is on top because it goes first;
// the experts are indented under it because it hands to them; the line is drawn
// when and only when it has handed to one, so the panel answers "what is
// happening right now" without a word of text.
//
// This is the same drawing the terminal makes in ui/widgets/expert_panel.cpp,
// turned ninety degrees. There the delegator sits level with the middle of the
// column and the connector reaches up and down; here it sits above and the
// connector only ever reaches down, because a window has the height to stack
// them and a terminal panel does not.
//
// Nothing in it holds state. The width and whether it is open live on App,
// because they are how this window is arranged rather than a preference.
#include "../app.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <imgui.h>

#include "../theme.hpp"
#include "../widgets.hpp"

namespace crucible::gui {
namespace {

/// The columns the tree is built on, in ems.
///
/// The delegator's dot sits on `kTrunk`, the experts' dots on `kBranch`, and
/// the connector is the elbow between the two. Writing them down once is what
/// keeps the vertical run underneath the dot it comes out of -- computed
/// separately in two places, they drift by a pixel and the line stops looking
/// like it is attached to anything.
constexpr float kTrunk  = 0.70F;
constexpr float kBranch = 2.05F;
constexpr float kLabel  = 2.95F;

/// What the delegator's own diamond should say.
///
/// Not a SeatPhase: the delegator does not have a seat. It has a model or it
/// does not, it is in memory or it is not, and it is either routing this
/// instant or it is not -- which is three states, and they map onto three of
/// the same five marks the experts use so that the column reads as one
/// vocabulary.
theme::Dot delegator_dot(const Config& config, const Snapshot& snapshot) {
    if (config.router.model.empty()) {
        return theme::Dot::Empty;
    }
    if (snapshot.mood == Mood::Routing) {
        return theme::Dot::Active;
    }
    return snapshot.delegator_ready ? theme::Dot::Ready : theme::Dot::Empty;
}

}  // namespace

// ---------------------------------------------------------------------------
// The tree
// ---------------------------------------------------------------------------

void App::draw_model_tree(const Snapshot& snapshot) {
    const Roster& roster = snapshot.roster ? *snapshot.roster : config_.roster;
    ImDrawList*   draw   = ImGui::GetWindowDrawList();
    const float   row    = em(1.55F);
    const float   dot    = em(0.30F);
    const float   line   = ImGui::GetTextLineHeight();
    const float   left   = ImGui::GetCursorScreenPos().x;
    const float   right  = left + ImGui::GetContentRegionAvail().x;

    // --- the delegator -----------------------------------------------------
    //
    // Both headings are indented to the column the names are in, which is what
    // leaves the trunk a clear run down the left. Set flush, "EXPERTS" is
    // crossed out by the very line that says work is flowing.
    //
    // Indent rather than a cursor position: section() opens with a Dummy, and
    // the item after a Dummy starts at the line's indent, not where the cursor
    // was put before it.
    ImGui::Indent(em(kLabel));
    section("DELEGATOR");
    ImGui::Unindent(em(kLabel));
    const bool   routing = snapshot.mood == Mood::Routing;
    const ImVec2 head    = ImGui::GetCursorScreenPos();
    const float  trunk_y = head.y + row * 0.5F;

    theme::draw_dot(draw, ImVec2(left + em(kTrunk), trunk_y), dot,
                    delegator_dot(config_, snapshot));

    {
        const std::string name = model_label(config_.router.model);
        const ImU32 ink = config_.router.model.empty() ? theme::kTextFaint
                        : routing                      ? theme::kFlameBright
                                                       : theme::kText;
        ImGui::PushFont(routing ? theme::bold() : theme::body());
        draw->AddText(ImVec2(left + em(kLabel), trunk_y - line * 0.5F), ink,
                      name.c_str());
        ImGui::PopFont();
    }
    // A hit target over the whole row, so the model behind the delegator is one
    // click away from the place it is shown rather than four clicks into
    // Settings for anybody who has not already learned where it lives.
    ImGui::InvisibleButton("##delegator", ImVec2(right - left, row));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s\n\nReads every prompt and picks the expert.\nClick to "
                          "change which model does it.",
                          config_.router.model.empty()
                              ? "No delegator model assigned."
                              : config_.router.model.c_str());
    }
    if (ImGui::IsItemActivated()) {
        show_settings(SettingsPage::General);
    }

    // --- the experts -------------------------------------------------------
    ImGui::Indent(em(kLabel));
    section("EXPERTS");
    ImGui::Unindent(em(kLabel));
    if (roster.experts().empty()) {
        wrapped(theme::kTextFaint, "None yet.");
    }

    // Where the line has to reach, filled in as the row that owns it is drawn.
    // -1 means nothing is running, which is the state the panel is in for most
    // of its life and the one where no line is drawn at all.
    float linked_y = -1.0F;

    for (std::size_t i = 0; i < roster.size(); ++i) {
        const Expert&    expert = roster.at(i);
        const SeatState& seat   = i < snapshot.seats.size() ? snapshot.seats[i]
                                                            : SeatState{};
        const bool linked = snapshot.linked && *snapshot.linked == expert.id;

        const ImVec2 at    = ImGui::GetCursorScreenPos();
        const float  mid   = at.y + row * 0.5F;
        if (linked) {
            linked_y = mid;
        }

        ImGui::PushID(static_cast<int>(i));
        ImGui::InvisibleButton("##seat", ImVec2(right - left, row));
        const bool hot = ImGui::IsItemHovered();
        if (hot) {
            draw->AddRectFilled(ImVec2(left + em(kBranch) - em(0.55F), at.y),
                                ImVec2(right, at.y + row), theme::kRaised, em(0.2F));
        }

        // Linked means this seat has the turn, which is what Active draws. The
        // phase can lag it by a moment -- the delegator names the seat before
        // the model is swapped in -- and a row whose name is yellow beside a
        // dot that is not says two different things about one expert.
        const theme::Dot mark = linked && seat.phase != SeatPhase::Loading
                                    ? theme::Dot::Active
                                    : dot_for(seat.phase);
        theme::draw_dot(draw, ImVec2(left + em(kBranch), mid), dot, mark);

        // Yellow is reserved for the one that has the turn -- the delegator
        // while it routes, the expert once it has been handed to. Everything
        // else is white if it can answer and faint if it cannot, so the eye
        // goes to the live one without having to compare five shades.
        std::string label = expert.name;
        if (seat.phase == SeatPhase::Loading) {
            label += "  " + std::to_string(static_cast<int>(seat.progress * 100.0F)) + "%";
        }
        const ImU32 ink = linked                                ? theme::kFlameBright
                        : seat.phase == SeatPhase::Unconfigured ? theme::kTextFaint
                        : seat.phase == SeatPhase::Missing      ? theme::kError
                                                                : theme::kText;
        ImGui::PushFont(linked ? theme::bold() : theme::body());
        draw->AddText(ImVec2(left + em(kLabel), mid - line * 0.5F), ink, label.c_str());
        ImGui::PopFont();

        if (hot) {
            const std::string& file = config_.expert(expert.id).model;
            ImGui::SetTooltip(
                "%s\n\n%s%s\n\nClick to assign a model to this expert.",
                expert.blurb.empty() ? expert.name.c_str() : expert.blurb.c_str(),
                file.empty() ? "No model assigned." : file.c_str(),
                seat.phase == SeatPhase::Missing
                    ? "\nThat file is not where the config says it is."
                    : "");
        }
        if (ImGui::IsItemActivated()) {
            show_settings(SettingsPage::Experts);
        }
        ImGui::PopID();
    }

    // --- the connector -----------------------------------------------------
    //
    // Drawn last, over the rows, because it crosses them: the vertical run
    // passes down the left of every expert between the delegator and the one
    // that was chosen. An elbow, exactly as the terminal draws it -- out of the
    // delegator, down the trunk, and in to the seat.
    if (linked_y > 0.0F) {
        const float x    = left + em(kTrunk);
        const float top  = trunk_y + dot + em(0.12F);
        const float turn = left + em(kBranch) - dot - em(0.18F);
        draw->AddLine(ImVec2(x, top), ImVec2(x, linked_y), theme::kFlameBright, 1.6F);
        draw->AddLine(ImVec2(x, linked_y), ImVec2(turn, linked_y),
                      theme::kFlameBright, 1.6F);
    }
}

// ---------------------------------------------------------------------------
// The panel around it
// ---------------------------------------------------------------------------

void App::draw_sidebar(const Snapshot& snapshot) {
    // Collapsed is a width, not a mode.
    //
    // The button in the top bar sets that width to zero and back; dragging the
    // splitter to the left edge does the same thing by hand. One property, two
    // ways to reach it, and no separate "is it open" flag to fall out of step
    // with the width.
    if (sidebar_collapsed()) {
        return;
    }

    ImGui::BeginChild("sidebar", ImVec2(sidebar_drawn_width(), 0),
                      ImGuiChildFlags_Borders);

    draw_model_tree(snapshot);

    ImGui::Dummy(ImVec2(0, em(0.4F)));
    if (ImGui::Button("Manage experts", ImVec2(-FLT_MIN, 0))) {
        show_settings(SettingsPage::Experts);
    }

    // --- the footer --------------------------------------------------------
    //
    // Pushed to the bottom by a filler rather than by an absolute cursor
    // position: the sidebar scrolls when the roster is long, and a footer
    // pinned to the window height ends up either overlapping the list or below
    // the visible area depending on how far it has scrolled.
    const TokenUsage& usage = snapshot.session_usage;
    const float footer = em(4.1F);
    const float slack  = ImGui::GetContentRegionAvail().y - footer;
    if (slack > 0.0F) {
        ImGui::Dummy(ImVec2(0, slack));
    }
    ImGui::Separator();
    text_coloured(theme::kTextFaint, "%s in / %s out",
                  format_tokens(usage.input_tokens).c_str(),
                  format_tokens(usage.output_tokens).c_str());
    ImGui::SetItemTooltip("Tokens this session. The project's running total is in "
                          "History.");

    ImGui::EndChild();
}

void App::show_settings(SettingsPage page) {
    if (view_ != View::Settings) {
        before_settings_ = view_;
    }
    view_          = View::Settings;
    settings_page_ = page;
}

// ---------------------------------------------------------------------------
// The splitter, and the collapse it owns
// ---------------------------------------------------------------------------
//
// `sidebar_width_` is what the user has dragged to, which is not always what
// gets drawn. Below sidebar_collapse_at() the sidebar is closed; between there
// and sidebar_min_width() it is drawn at the minimum. The gap between the two
// is deliberate: it takes a deliberate overshoot to close the panel, so it
// cannot slam shut on one pixel of movement while you are trimming its width,
// and there is a narrowest width you can rest at.

float App::sidebar_min_width() const   { return em(13.0F); }
float App::sidebar_collapse_at() const { return em(8.0F); }

bool App::sidebar_collapsed() const {
    return sidebar_width_ < sidebar_collapse_at();
}

float App::sidebar_drawn_width() const {
    return std::max(sidebar_width_, sidebar_min_width());
}

void App::draw_splitter() {
    const bool collapsed = sidebar_collapsed();
    if (collapsed) {
        // Nothing to grab and nothing to grab it from: the fold button in the
        // top bar is how a closed side menu comes back now, which is where
        // every other application keeps it and is a target you can hit without
        // knowing it is there. The bare edge with its little grip is gone.
        return;
    }
    ImGui::SameLine(0.0F, 0.0F);

    // An invisible button dragged sideways. ImGui has no splitter widget, and
    // this is what one is: a thing that is hovered, held, and reports how far
    // the mouse moved while it was held.
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::to_vec(theme::kFlame));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::to_vec(theme::kFlameBright));
    ImGui::Button("##splitter", ImVec2(em(0.35F), -FLT_MIN));
    ImGui::PopStyleColor(3);

    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    ImGui::SetItemTooltip("Drag to resize -- drag to the edge to close");

    if (ImGui::IsItemActive()) {
        sidebar_width_ += ImGui::GetIO().MouseDelta.x;
        // Remembered while it is being dragged, so closing it by dragging and
        // reopening it from the top bar lands back on this width rather than on
        // the zero the drag finished at.
        if (!sidebar_collapsed()) {
            sidebar_restore_ = sidebar_width_;
        }
    }
    // Clamped so it can be neither dragged past the left edge into negative
    // width nor pulled over the whole window. Zero is a real, reachable value:
    // it is what closed means.
    const float most = std::max(em(14.0F), ImGui::GetWindowWidth() * 0.45F);
    sidebar_width_ = std::clamp(sidebar_width_, 0.0F, most);

    ImGui::SameLine(0.0F, 0.0F);
}

}  // namespace crucible::gui
