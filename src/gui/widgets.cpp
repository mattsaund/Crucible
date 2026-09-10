// SPDX-License-Identifier: MIT
#include "widgets.hpp"

#include <imgui_stdlib.h>

#include <algorithm>
#include <cstdarg>
#include <system_error>

#include "theme.hpp"

namespace crucible::gui {

float em(float n) {
    return n * ImGui::GetFontSize();
}

void text_colored(ImU32 color, const char* fmt, ...) IM_FMTARGS(2);
void text_colored(ImU32 color, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::to_vec(color));
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

void wrapped(ImU32 color, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::to_vec(color));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

void section(const char* label) {
    ImGui::Dummy(ImVec2(0, em(0.5F)));
    text_colored(theme::kTextFaint, "%s", label);
    ImGui::Dummy(ImVec2(0, em(0.1F)));
}

void title(const char* label) {
    ImGui::PushFont(theme::heading());
    text_colored(theme::kFlame, "%s", label);
    ImGui::PopFont();
}

float grow_input_height(const std::string& text, float width, int max_lines) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float       line  = ImGui::GetTextLineHeight();

    // Measured at the width the text will actually wrap at, so the box is as
    // tall as the content needs and not as tall as the character count guesses.
    const float inner = std::max(width - style.FramePadding.x * 2.0F, line);
    float       tall  = line;
    if (!text.empty()) {
        tall = ImGui::CalcTextSize(text.c_str(), text.c_str() + text.size(),
                                   false, inner).y;
        // A trailing newline is a line the user has started and CalcTextSize
        // does not count, which would make the caret sit outside the box.
        if (text.back() == '\n') {
            tall += line;
        }
    }
    const float ceiling = line * static_cast<float>(std::max(max_lines, 1));
    return std::clamp(tall, line, ceiling) + style.FramePadding.y * 2.0F;
}

bool grow_input(const char* id, const char* hint, std::string& text,
                float width, int max_lines, float height) {
    const ImGuiStyle& style  = ImGui::GetStyle();
    const ImVec2      origin = ImGui::GetCursorScreenPos();
    // A height the caller asked for wins over the measured one. One line is the
    // floor either way: a box shorter than its own caret cannot be typed in.
    const float tall = height > 0.0F
                           ? std::max(height, ImGui::GetTextLineHeight()
                                                  + style.FramePadding.y * 2.0F)
                           : grow_input_height(text, width, max_lines);

    const bool submitted = ImGui::InputTextMultiline(
        id, &text, ImVec2(width, tall),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);

    // InputTextMultiline has no WithHint form, so the placeholder is painted
    // over the empty box rather than passed in.
    if (text.empty() && hint != nullptr) {
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(origin.x + style.FramePadding.x * 2.0F,
                   origin.y + style.FramePadding.y),
            theme::kTextFaint, hint);
    }
    return submitted;
}

float reading_column(float available) {
    // One measurement of one glyph: the face is monospace, so every column is
    // this wide.
    const float column = ImGui::CalcTextSize("0").x;
    return column > 0.0F ? std::min(available, column * 110.0F) : available;
}

IconHit icon_slot(const char* id, float size, bool lit) {
    IconHit hit;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(size, size));
    hit.clicked = ImGui::IsItemActivated();
    hit.hovered = ImGui::IsItemHovered();
    hit.center  = ImVec2(at.x + size * 0.5F, at.y + size * 0.5F);

    // The plate is what makes a drawn mark feel like a button. Two states only:
    // under the pointer, and holding the view that is on screen. A third for
    // "pressed" would be visible for one frame of a click.
    if (hit.hovered || lit) {
        const float inset = size * 0.10F;
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(at.x + inset, at.y + inset),
            ImVec2(at.x + size - inset, at.y + size - inset),
            lit ? IM_COL32(0xFF, 0x87, 0x00, 0x28) : theme::kRaised, size * 0.16F);
    }
    return hit;
}

bool top_tab(const char* label, bool selected, float height) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float width = ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.6F;
    const ImVec2 at   = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(label, ImVec2(width, height));
    const bool pressed = ImGui::IsItemActivated();
    const bool hot     = ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hot && !selected) {
        draw->AddRectFilled(ImVec2(at.x, at.y + height * 0.16F),
                            ImVec2(at.x + width, at.y + height * 0.84F),
                            theme::kRaised, height * 0.10F);
    }
    const ImU32 ink = selected ? theme::kText : hot ? theme::kTextDim : theme::kTextFaint;
    const ImVec2 size = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(at.x + (width - size.x) * 0.5F,
                         at.y + (height - size.y) * 0.5F), ink, label);

    // The bar sits on the bottom edge of the bar itself, so the three tabs read
    // as one strip with one of them underlined rather than as three buttons.
    if (selected) {
        draw->AddRectFilled(ImVec2(at.x + width * 0.12F, at.y + height - 2.0F),
                            ImVec2(at.x + width * 0.88F, at.y + height),
                            theme::kFlame);
    }
    return pressed;
}

std::string model_label(const std::string& reference) {
    if (reference.empty()) {
        return "(none)";
    }
    const std::size_t slash = reference.find_last_of("/\\");
    return slash == std::string::npos ? reference : reference.substr(slash + 1);
}

std::string tail_of(const std::filesystem::path& path, std::size_t width) {
    const std::string text = path.string();
    if (text.size() <= width) {
        return text;
    }
    return "..." + text.substr(text.size() - width);
}

theme::Dot dot_for(SeatPhase phase) {
    switch (phase) {
        case SeatPhase::Active:       return theme::Dot::Active;
        case SeatPhase::Loading:      return theme::Dot::Loading;
        case SeatPhase::Dormant:      return theme::Dot::Ready;
        case SeatPhase::Missing:      return theme::Dot::Missing;
        case SeatPhase::Unconfigured: break;
    }
    return theme::Dot::Empty;
}

const char* mood_text(Mood mood) {
    switch (mood) {
        case Mood::Routing:  return "routing";
        case Mood::Loading:  return "loading";
        case Mood::Thinking: return "thinking";
        case Mood::Talking:  return "answering";
        case Mood::Error:    return "error";
        case Mood::Idle:     break;
    }
    return "idle";
}

ImU32 step_color(const CookStep& step) {
    if (!step.ok) {
        return theme::kError;
    }
    if (step.kind == "write") {
        return theme::kAdded;
    }
    if (step.kind == "run" || step.kind == "done" || step.kind == "handoff") {
        return theme::kFlame;
    }
    return theme::kTextFaint;
}

std::vector<std::filesystem::path> subdirectories(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (!name.empty() && name.front() == '.') {
            continue;  // a project picker full of .cache and .git helps nobody
        }
        found.push_back(entry.path());
    }
    std::sort(found.begin(), found.end());
    return found;
}

}  // namespace crucible::gui
