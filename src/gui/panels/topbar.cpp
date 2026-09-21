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
namespace {

/// A string cut to fit `room` pixels, with an ellipsis where it was cut.
///
/// The face is monospace, so this is arithmetic rather than a search: one
/// measurement gives the width of every column.
std::string elide(const std::string& text, float room, float advance) {
    if (advance <= 0.0F) {
        return text;
    }
    const auto fits = static_cast<std::size_t>(std::max(room / advance, 0.0F));
    if (text.size() <= fits) {
        return text;
    }
    if (fits < 2) {
        return {};
    }
    // Kept from the right. A path is identified by its tail -- two projects
    // under the same parent differ in the last component and agree on every
    // one before it.
    return "\xE2\x80\xA6" + text.substr(text.size() - (fits - 1));
}

/// The same, but the cut is taken out of the middle.
///
/// For a path, which is identified at both ends and at neither of the places in
/// between: the head says which disk or which home directory, the tail says
/// which project, and the six nested directories in the middle are the part
/// that can go. Cutting from the left alone loses the first and cutting from
/// the right loses the second.
std::string middle_out(const std::string& text, float room, float advance) {
    if (advance <= 0.0F) {
        return text;
    }
    const auto fits = static_cast<std::size_t>(std::max(room / advance, 0.0F));
    if (text.size() <= fits) {
        return text;
    }
    if (fits < 8) {
        return elide(text, room, advance);   // too little room to keep two ends
    }
    const std::size_t keep = fits - 1;       // one column for the ellipsis
    const std::size_t tail = keep * 2 / 3;   // the end is worth more than the start
    const std::size_t head = keep - tail;
    return text.substr(0, head) + "\xE2\x80\xA6" + text.substr(text.size() - tail);
}

}  // namespace

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
    const float  advance = ImGui::CalcTextSize("0").x;
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
    theme::draw_flame(draw, ImVec2(origin.x + mark_x + em(0.45F), middle), em(0.62F));
    ImGui::SetCursorPosX(mark_x + em(1.15F));

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
    // Centered in the bar, because it is what the whole window is about and the
    // controls on either side are not. Said in words rather than behind a
    // folder glyph: a small drawn icon next to a path is decoration that has to
    // be decoded, and "Project:" is four letters that never has to be.
    //
    // Measured first and placed from the middle out, so the path grows in both
    // directions and the label stays where it was. It gives up the middle of
    // the path rather than the end when there is not room -- the beginning says
    // which disk and the end says which project; it is the part between them
    // that nobody reads.
    {
        const std::string path  = store_->project().root.string();
        const float       start = ImGui::GetCursorPosX();
        const float       stop  = width - right_room;

        const std::string label = "Project: ";
        const float label_w  = ImGui::CalcTextSize(label.c_str()).x;
        const float button_w = ImGui::CalcTextSize("Change project").x
                             + style.FramePadding.x * 2.0F;
        const float gap      = em(0.8F);

        const float room = std::max(stop - start - em(1.2F), em(8.0F));
        const std::string shown =
            middle_out(path, room - label_w - button_w - gap, advance);
        const float block = label_w + ImGui::CalcTextSize(shown.c_str()).x
                          + gap + button_w;

        // Centered on the window, then pushed right if the wordmark and the
        // fold button have already taken that space. It never overlaps either
        // side; it only stops being centered.
        float at_x = std::max((width - block) * 0.5F, start);
        at_x = std::min(at_x, std::max(stop - block, start));

        const ImVec2 at = ImVec2(origin.x + at_x, middle - line * 0.5F);
        draw->AddText(at, theme::kTextFaint, label.c_str());
        draw->AddText(ImVec2(at.x + label_w, at.y), theme::kText, shown.c_str());

        ImGui::SetCursorPosX(at_x + block - button_w);
        center_y(ImGui::GetFrameHeight());
        if (ImGui::Button("Change project")) {
            open_browse(BrowseFor::Project, store_->project().root);
        }
        ImGui::SetItemTooltip("%s", path.c_str());
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
            tab.view == View::Chat    ? "Ask, and the delegator picks the expert"
          : tab.view == View::Cook    ? "Set one goal and let the experts work it in passes"
          : tab.view == View::Create  ? "Fine-tune a model on your own data until it is an expert in one subject"
                                      : "Everything this project has done");
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
        ImGui::SetItemTooltip("Settings");
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
