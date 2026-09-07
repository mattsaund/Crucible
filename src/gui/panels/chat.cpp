// SPDX-License-Identifier: MIT
//
// The chat transcript and the box you type into.
//
// Chat is the plainer of the two working views and is meant to be: you ask, the
// delegator reads the question and picks an expert, the expert answers with the
// tools it has. One turn is that whole story -- the question, who took it, what
// it looked up on the way, and the answer -- and the layout is built so those
// four read in that order without a label on any of them.
//
// The composer is here rather than with the frame because what it does depends
// on which view is showing: in Chat it sends a prompt, in Cook it starts or
// stops a cook.
#include "../app.hpp"

#include <algorithm>
#include <string>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "../markdown_view.hpp"
#include "../theme.hpp"
#include "../widgets.hpp"
#include "crucible/util/format.hpp"

namespace crucible::gui {
namespace {

/// What the user said, in a plate of its own.
///
/// A transcript is two voices and they have to be told apart at a glance while
/// scrolling. The reply is the page -- full width, full markdown, whatever
/// length it needs -- so the question is what gets the treatment: inset, on a
/// raised ground, with the flame down its left edge.
///
/// The plate is drawn before the text rather than around it, which is why the
/// height is measured first. ImGui draws in call order and has no way to put a
/// rectangle behind something already on the screen.
void prompt_plate(const std::string& text) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float pad_x = em(0.8F);
    const float pad_y = em(0.45F);
    const float width = ImGui::GetContentRegionAvail().x;
    const float wrap  = width - pad_x * 2.0F - em(0.3F);

    const float tall = ImGui::CalcTextSize(text.c_str(), text.c_str() + text.size(),
                                           false, wrap).y;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList*  draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, ImVec2(at.x + width, at.y + tall + pad_y * 2.0F),
                        theme::kRaised, style.ChildRounding);
    draw->AddRectFilled(at, ImVec2(at.x + em(0.16F), at.y + tall + pad_y * 2.0F),
                        theme::kFlame, style.ChildRounding,
                        ImDrawFlags_RoundCornersLeft);

    ImGui::SetCursorScreenPos(ImVec2(at.x + pad_x, at.y + pad_y));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::to_vec(theme::kText));
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + tall + pad_y * 2.0F));
}

/// Who took the turn, and on what grounds.
///
/// The confidence is a percentage rather than the two decimal places it used to
/// be: 0.94 is a number to be interpreted and 94% is one to be read, and the
/// difference between 0.94 and 0.93 was never going to change anybody's mind.
void route_line(const Roster& roster, const Turn& turn) {
    if (!turn.route) {
        return;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float line = ImGui::GetTextLineHeight();
    const ImVec2 at  = ImGui::GetCursorScreenPos();
    theme::draw_dot(draw, ImVec2(at.x + em(0.3F), at.y + line * 0.5F), em(0.26F),
                    theme::Dot::Active);
    ImGui::Dummy(ImVec2(em(0.85F), line));
    ImGui::SameLine(0.0F, 0.0F);

    ImGui::PushFont(theme::bold());
    text_coloured(theme::kFlameBright, "%s",
                  expert_label(roster, turn.route->expert).c_str());
    ImGui::PopFont();
    ImGui::SameLine();

    std::string rest = "  \xC2\xB7  "
                     + std::to_string(static_cast<int>(turn.route->confidence * 100.0F))
                     + "%  \xC2\xB7  " + std::string(route_source_name(turn.route->source));
    if (turn.load_ms > 0) {
        rest += "  \xC2\xB7  swapped in " + format::duration_ms(turn.load_ms);
    }
    text_coloured(theme::kTextFaint, "%s", rest.c_str());
    ImGui::SetItemTooltip("How sure the delegator was, and what decided it.");
}

}  // namespace

// ---------------------------------------------------------------------------
// The empty state
// ---------------------------------------------------------------------------

void App::draw_readiness() {
    // Which of the three, and where it is fixed.
    //
    // The order is the order the machine gets built up in: without a backend
    // nothing can run at all, and with one but no model there is nothing to
    // run. Only the last is the invitation, and only the last has nowhere to
    // send you.
    const char*  label   = "Ask anything";
    SettingsPage page    = SettingsPage::General;
    bool         fixable = true;
    if (!any_runtime_) {
        label = "No runtime";
        page  = SettingsPage::Runtimes;
    } else if (config_.configured_experts().empty()) {
        label = "No model selected";
        page  = SettingsPage::Experts;
    } else {
        fixable = false;
    }

    const float room = ImGui::GetContentRegionAvail().x;
    const auto  centre = [&room](float item) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max((room - item) * 0.5F, 0.0F));
    };

    ImGui::Dummy(ImVec2(0, em(4.0F)));

    ImGui::PushFont(theme::heading());
    const ImVec2 size = ImGui::CalcTextSize(label);
    centre(size.x);
    text_coloured(fixable ? theme::kTextDim : theme::kTextFaint, "%s", label);
    ImGui::PopFont();

    // The button says Settings and nothing else. It is the second and last
    // thing on the screen, so what it does has to be readable from the two
    // words above it -- "No runtime / Settings" is a sentence; "No runtime /
    // Install a Vulkan or CUDA backend" is a paragraph pretending to be a
    // button.
    if (fixable) {
        ImGui::Dummy(ImVec2(0, em(0.9F)));
        const float width = em(8.0F);
        centre(width);
        if (ImGui::Button("Settings", ImVec2(width, 0))) {
            show_settings(page);
        }
    }
}

void App::draw_chat(const Snapshot& snapshot) {
    for (const std::string& notice : notices_) {
        wrapped(theme::kTextDim, "- " + notice);
    }
    if (!notices_.empty()) {
        ImGui::Dummy(ImVec2(0, em(0.5F)));
    }

    if (snapshot.turns.empty()) {
        draw_readiness();
    }

    const Roster& roster = snapshot.roster ? *snapshot.roster : config_.roster;

    for (std::size_t i = 0; i < snapshot.turns.size(); ++i) {
        const Turn& turn = snapshot.turns[i];
        // The ID a turn's code blocks hang off. Without it every block in the
        // transcript shares one state, and expanding a long one in the third
        // reply expands one in the first.
        ImGui::PushID(static_cast<int>(i));

        prompt_plate(turn.prompt);
        ImGui::Dummy(ImVec2(0, em(0.45F)));

        route_line(roster, turn);

        // What the turn did besides talk, before the answer that used it: a page
        // fetched, a file read or rewritten, a command run. Shown whether or not
        // the reply mentions them -- a model that edits one file and writes a
        // summary of editing another is not rare, and this is what catches it.
        for (const std::string& action : turn.actions) {
            text_coloured(theme::kTextDim, "   \xC2\xB7  %s", action.c_str());
        }

        if (config_.ui.show_reasoning && !turn.reasoning.empty()) {
            ImGui::Dummy(ImVec2(0, em(0.3F)));
            text_coloured(theme::kTextFaint, "thinking");
            ImGui::PushFont(theme::italic());
            wrapped(theme::kTextFaint, turn.reasoning);
            ImGui::PopFont();
        }

        ImGui::Dummy(ImVec2(0, em(0.35F)));

        // Rendered, not printed. Every instruction-tuned model answers in
        // markdown whether or not you ask it to, and shown raw that is a wall
        // of asterisks with the structure left for the reader to reconstruct.
        draw_markdown(turn.reply, turn.failed ? theme::kError : theme::kText);

        if (turn.streaming && turn.reply.empty()) {
            text_coloured(theme::kTextFaint, "...");
        }
        if (turn.cancelled) {
            text_coloured(theme::kTextFaint, "-- stopped");
        }

        if (turn.tokens_per_second > 0.0) {
            ImGui::Dummy(ImVec2(0, em(0.2F)));
            text_coloured(theme::kTextFaint, "%s tok/s  \xC2\xB7  %d in  \xC2\xB7  %d out",
                          format::number(turn.tokens_per_second, 1).c_str(),
                          turn.prompt_tokens, turn.output_tokens);
        }
        ImGui::Dummy(ImVec2(0, em(0.7F)));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, em(0.5F)));
        ImGui::PopID();
    }

    if (snapshot.cook) {
        draw_cook(snapshot);
    }
}

/// The chat box: one prompt, nothing else.
///
/// Cooking has its own screen and its own controls. Putting both on one bar
/// meant every prompt was typed next to a Cook button that would have thrown
/// the prompt away, which is a question the user should not be asked to answer
/// on the way to sending a message.
void App::draw_chat_composer(const Snapshot& snapshot) {
    const std::shared_ptr<const Cook> cook = snapshot.cook;
    const bool asking  = cook && cook->state == CookState::Asking;
    const bool cooking = engine_->cooking();

    ImGui::BeginChild("chat-composer", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);

    const char* hint = asking  ? "answer the question on the cook screen"
                     : cooking ? "cooking -- ask anyway and it waits its turn"
                               : "ask anything";

    // The box lines up with the answers above it. The bar reaches both edges
    // like the panel over it, and what you type sits in the same column the
    // replies are set in -- a prompt box twice the width of the reply to it
    // reads as belonging to a different program.
    const float room   = ImGui::GetContentRegionAvail().x;
    const float column = reading_column(room);
    if (column < room) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (room - column) * 0.5F);
    }
    const float button = em(5.0F);
    const float width  = std::max(column - button - ImGui::GetStyle().ItemSpacing.x,
                                  em(6.0F));
    const bool entered = grow_input("##prompt", hint, prompt_, width, kComposerLines,
                                    composer_input_height());
    ImGui::SameLine();
    const bool send = ImGui::Button(asking ? "Answer" : "Send", ImVec2(button, 0));
    if (entered || send) {
        submit_prompt();
        // Enter should leave the caret where it was, or every reply costs a
        // click to get back to typing.
        ImGui::SetKeyboardFocusHere(-1);
    }

    ImGui::EndChild();
}

}  // namespace crucible::gui
