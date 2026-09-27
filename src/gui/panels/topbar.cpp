// SPDX-License-Identifier: MIT
//
// The bar across the top: where you are, and which of the three views you are
// looking at.
//
// Everything in it answers a question about the whole window rather than about
// what is on screen, which is the line that decides what goes here and what
// goes in a panel. Which project is open, whether the side menu is showing,
// Chat or Cook or History, and the way through to Settings -- all four are true
// of the window, not of the transcript.
//
// It is laid out from both ends inwards. The right-hand group is measured
// first, because the tabs and the gear must not move as a project name gets
// longer; the left-hand group then gets whatever is left and gives up its
// pieces in order -- the status, then the path, then the wordmark -- as the
// window narrows. Nothing overlaps at any width, which a row of ImGui::SameLine
// calls cannot promise.
#include "../app.hpp"

#include <algorithm>
#include <string>

#include <imgui.h>

#include "../theme.hpp"
#include "../widgets.hpp"

namespace crucible::gui {

void App::draw_topbar() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float bar    = topbar_height();
    const float square = em(1.9F);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::to_vec(theme::kPanel));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(0.5F), 0.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(em(0.35F), 0.0F));
    ImGui::BeginChild("topbar", ImVec2(0, bar), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList*  draw    = ImGui::GetWindowDrawList();
    const ImVec2 origin  = ImGui::GetWindowPos();
    const float  width   = ImGui::GetWindowWidth();
    const float  middle  = origin.y + bar * 0.5F;
    const float  line    = ImGui::GetTextLineHeight();

    // The edge under the bar, which is what separates it from the panels
    // rather than a border around a child that would box it in on all sides.
    draw->AddLine(ImVec2(origin.x, origin.y + bar - 1.0F),
                  ImVec2(origin.x + width, origin.y + bar - 1.0F), theme::kPanelEdge);

    const auto center_y = [&](float height) {
        ImGui::SetCursorPosY((bar - height) * 0.5F);
    };

    // --- the right-hand group, measured before anything is drawn -----------
    //
    // Settings is a corner, not a tab. It is where you go to change the program
    // rather than one of the three things the program does, and putting it
    // fourth in the row would make "am I in Settings" a thing you have to
    // check before typing.
    struct Tab { const char* label; View view; };
    const Tab tabs[] = {
        {"Chat", View::Chat}, {"Cook", View::Cook}, {"Create", View::Create},
        {"History", View::History},
    };
    float tab_room = 0.0F;
    for (const Tab& tab : tabs) {
        tab_room += ImGui::CalcTextSize(tab.label).x + style.FramePadding.x * 2.6F
                  + style.ItemSpacing.x;
    }
    const float right_room = tab_room + square + em(0.8F);

    // --- the left-hand group -----------------------------------------------
    ImGui::SetCursorPosX(em(0.3F));
    center_y(square);
    {
        const bool open = !sidebar_collapsed();
        const IconHit hit = icon_slot("##fold", square);
        theme::draw_panel_icon(draw, hit.center, em(1.05F),
                               hit.hovered ? theme::kFlameBright : theme::kTextDim, open);
        ImGui::SetItemTooltip(open ? "Hide the side menu" : "Show the side menu");
        if (hit.clicked) {
            toggle_sidebar();
        }
    }

    // The mark. Small, and the one place in the window it appears now that the
    // side menu is a list of models rather than a masthead.
    ImGui::SameLine();
    const float mark_x = ImGui::GetCursorPosX();
    theme::draw_mark(draw, ImVec2(origin.x + mark_x + em(0.85F), middle), em(1.9F));
    ImGui::SetCursorPosX(mark_x + em(2.0F));

    float left_used = ImGui::GetCursorPosX();
    const float left_room = width - right_room - left_used;

    // "CRUCIBLE", the first thing to go when the window is narrow: the flame
    // beside it says the same thing in a fifth of the room.
    if (left_room > em(22.0F)) {
        ImGui::PushFont(theme::bold());
        center_y(line);
        text_colored(theme::kText, "CRUCIBLE");
        ImGui::PopFont();
        ImGui::SameLine();
    }

    // --- the project -------------------------------------------------------
    //
    // One chip in the middle of the bar, holding the folder's name, and
    // clicking it is how you change project.
    //
    // It used to be the whole path -- "Project: /home/matt/Desktop/Code/Git/
    // owner/public/Crucible" -- with a Change project button after it, and
    // between them they took half the bar to say something the title of every
    // other window says in one word. The path still matters, so it is the
    // tooltip: there when you ask which checkout this is, gone the rest of the
    // time. The chip doubles as the button because "the thing that says which
    // project" and "the thing that changes project" being two separate targets
    // was one target too many.
    {
        const bool        open = project_open();
        const std::string path = project_root().string();
        const std::string name = open ? project_root().filename().string() : std::string();
        const std::string label =
            open ? (name.empty() ? path : name) : std::string("Open Project");

        const float start = ImGui::GetCursorPosX();
        const float stop  = width - right_room;
        const float room  = std::max(stop - start - em(1.2F), em(6.0F));

        const float pad   = style.FramePadding.x * 2.5F;
        const std::string shown = middle_out(label, room - pad);
        const float block = ImGui::CalcTextSize(shown.c_str()).x + pad;

        // Centered on the window, then pushed right if the wordmark has already
        // taken that space. It never overlaps either side; it only stops being
        // centered.
        float at_x = std::max((width - block) * 0.5F, start);
        at_x = std::min(at_x, std::max(stop - block, start));

        ImGui::SetCursorPosX(at_x);
        center_y(ImGui::GetFrameHeight());
        // Flat until hovered: at rest it reads as a title, and it only offers
        // itself as a control once the pointer is on it.
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              theme::to_vec(open ? theme::kText : theme::kTextDim));
        if (ImGui::Button(shown.c_str(), ImVec2(block, 0))) {
            open_browse(BrowseFor::Project, project_root());
        }
        ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("%s", open ? path.c_str() : "Choose a folder to work in");
        left_used = at_x + block;
    }

    // What it is doing is not here any more; it is in the side menu, over the
    // column of models it is a sentence about. The bar is for things true of
    // the whole window, and "loading gpt-oss-20b 40%" is true of one model for
    // thirty seconds -- it was also the one message with a number in it and the
    // one most likely to be cut, since it was elided to whatever was left
    // between the project path and the tabs.

    // --- the tabs and the gear ---------------------------------------------
    ImGui::SetCursorPosX(width - right_room);
    center_y(bar);
    for (const Tab& tab : tabs) {
        if (top_tab(tab.label, view_ == tab.view, bar)) {
            view_ = tab.view;
        }
        ImGui::SetItemTooltip(
            tab.view == View::Chat    ? "Ask one question"
          : tab.view == View::Cook    ? "Work one goal in passes"
          : tab.view == View::Create  ? "Fine-tune an expert"
                                      : "Past cooks and conversations");
        ImGui::SameLine();
    }

    ImGui::SetCursorPosX(width - square - em(0.4F));
    center_y(square);
    {
        const IconHit hit = icon_slot("##settings", square, view_ == View::Settings);
        theme::draw_gear(draw, hit.center, em(1.05F),
                         view_ == View::Settings ? theme::kFlame
                         : hit.hovered           ? theme::kFlameBright
                                                 : theme::kTextDim);

        // A newer Crucible exists: one dot on the gear, because that is where
        // the rest of the sentence is. A banner across the top bar would be a
        // notification about the program interrupting the work the program is
        // for, and this is news that keeps for a week.
        if (update_available()) {
            const float r = em(0.22F);
            draw->AddCircleFilled(
                ImVec2(hit.center.x + em(0.62F), hit.center.y - em(0.55F)), r,
                theme::kFlame);
        }
        if (update_available()) {
            ImGui::SetItemTooltip("Settings  --  Crucible %s is available",
                                  update_.latest.c_str());
        } else {
            ImGui::SetItemTooltip("Settings");
        }
        if (hit.clicked) {
            // Back out of Settings to whichever view was showing before it, so
            // the gear is a way in and out rather than a one-way door that
            // leaves Chat and Cook to be found again by hand.
            if (view_ == View::Settings) {
                view_ = before_settings_;
            } else {
                show_settings(settings_page_);
            }
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

float App::topbar_height() const {
    return em(2.6F);
}

void App::toggle_sidebar() {
    if (sidebar_collapsed()) {
        sidebar_width_ = std::max(sidebar_restore_, sidebar_min_width());
        return;
    }
    // Remembered, so bringing it back puts it where it was rather than at some
    // default the user has already dragged away from twice.
    sidebar_restore_ = sidebar_width_;
    sidebar_width_   = 0.0F;
}

}  // namespace crucible::gui
