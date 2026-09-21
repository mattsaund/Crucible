// SPDX-License-Identifier: MIT
//
// The cook view and the history of past cooks.
//
// Cook is the other half of the program, and it is not a longer chat. One goal
// is set once, and then the experts work it in passes: read, change, run, judge
// what came out, and go round again -- handing off to a different expert
// whenever the work turns into a different kind of work. Each pass is meant to
// leave the project better than the one before it, and the screen is built to
// show that: the goal fixed at the top, the pass count next to it, and the
// steps accumulating underneath in the order they happened.
//
// A step is a verb, a summary, and the diff or output it produced. Steps are
// folded by default and expand in place: an hour of cooking is hundreds of
// them, and a wall of diffs is not a progress report.
//
// History is here because it reads the same journals -- a finished cook and a
// running one differ only in whether anything is still being appended.
#include "../app.hpp"

#include <algorithm>
#include <string>

#include <imgui.h>

#include "../markdown_view.hpp"
#include "../theme.hpp"
#include "../widgets.hpp"

namespace crucible::gui {

void App::draw_cook_step(const CookStep& step, std::size_t index) {
    if (expanded_.size() <= index) {
        expanded_.resize(index + 1, false);
    }

    ImGui::PushID(static_cast<int>(index));

    ImDrawList*  draw   = ImGui::GetWindowDrawList();
    const float  line   = ImGui::GetTextLineHeight();
    const float  badge  = em(4.6F);
    const bool   has    = !step.detail.empty();
    const ImU32  ink    = step_color(step);

    const ImVec2 at    = ImGui::GetCursorScreenPos();
    const float  width = ImGui::GetContentRegionAvail().x;
    const float  row   = line + em(0.42F);

    // The whole row is the hit target when there is something to open. A step
    // with no detail is not a control and must not light up under the pointer
    // as though it were.
    if (has) {
        ImGui::InvisibleButton("##step", ImVec2(width, row));
        if (ImGui::IsItemActivated()) {
            expanded_[index] = !expanded_[index];
        }
        if (ImGui::IsItemHovered()) {
            draw->AddRectFilled(at, ImVec2(at.x + width, at.y + row), theme::kRaised,
                                em(0.2F));
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        theme::draw_chevron(draw, ImVec2(at.x + em(0.5F), at.y + row * 0.5F),
                            line * 0.7F, theme::kTextFaint, expanded_[index]);
    } else {
        ImGui::Dummy(ImVec2(width, row));
    }

    // The verb, as a badge rather than a padded word. A hundred steps read as a
    // list because the badges line up; they used to be a fixed-width string,
    // which lines up only as long as no verb is longer than the padding.
    const float  badge_y = at.y + (row - line) * 0.5F;
    const ImVec2 badge_at(at.x + em(1.05F), badge_y - em(0.06F));
    draw->AddRectFilled(badge_at,
                        ImVec2(badge_at.x + badge, badge_at.y + line + em(0.12F)),
                        (ink & 0x00FFFFFFU) | 0x22000000U, em(0.18F));
    const ImVec2 verb_size = ImGui::CalcTextSize(step.kind.c_str());
    draw->AddText(ImVec2(badge_at.x + (badge - verb_size.x) * 0.5F, badge_y), ink,
                  step.kind.c_str());

    draw->AddText(ImVec2(at.x + em(1.05F) + badge + em(0.6F), badge_y),
                  step.ok ? theme::kText : theme::kError, step.summary.c_str());

    if (expanded_[index] && has) {
        ImGui::Indent(em(1.05F));
        // The file the step touched, so a diff says which file it is a diff of
        // even when the tool that produced it did not write a header.
        const std::string_view path = step.changed.empty()
                                          ? std::string_view()
                                          : std::string_view(step.changed.front());
        draw_code_block(step.detail, path, path, 0);
        ImGui::Unindent(em(1.05F));
        ImGui::Dummy(ImVec2(0, em(0.3F)));
    }
    ImGui::PopID();
}

void App::draw_cook(const Snapshot& snapshot) {
    if (!snapshot.cook) {
        // The same line Chat shows, and for the same reason: what is missing is
        // missing for both, and the box below already says what this screen
        // takes. A paragraph about passes and handoffs told nobody anything
        // they could not see the first time they used it.
        draw_readiness();
        return;
    }
    const Cook& cook = *snapshot.cook;

    // --- the goal ----------------------------------------------------------
    //
    // Pinned at the top of everything the cook has done, because it is the one
    // thing that does not change while it runs and the one thing every step
    // underneath has to be judged against.
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float width = ImGui::GetContentRegionAvail().x;
        const float pad   = em(0.7F);
        const float wrap  = width - pad * 2.0F;
        const float tall  = ImGui::CalcTextSize(cook.goal.c_str(),
                                                cook.goal.c_str() + cook.goal.size(),
                                                false, wrap).y;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float  box = tall + pad * 2.0F + ImGui::GetTextLineHeight() + em(0.3F);
        ImDrawList*  draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(at, ImVec2(at.x + width, at.y + box), theme::kRaised,
                            style.ChildRounding);
        draw->AddRectFilled(at, ImVec2(at.x + em(0.16F), at.y + box), theme::kFlame,
                            style.ChildRounding, ImDrawFlags_RoundCornersLeft);

        ImGui::SetCursorScreenPos(ImVec2(at.x + pad, at.y + pad * 0.7F));
        text_colored(theme::kTextFaint, "GOAL");
        ImGui::SetCursorScreenPos(ImVec2(at.x + pad, at.y + pad * 0.7F
                                                   + ImGui::GetTextLineHeight() + em(0.2F)));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::to_vec(theme::kText));
        ImGui::TextUnformatted(cook.goal.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();

        // Stop, on the block it would stop. The composer at the bottom has the
        // same two buttons and always will -- they are the deliberate,
        // labeled way out -- but a cook that has scrolled a hundred steps
        // past its own goal should not need to be scrolled back to to be
        // ended, and the goal card is the thing on screen that *is* the cook.
        const bool live = cook.state == CookState::Working
                       || cook.state == CookState::Finishing;
        if (live && ImGui::IsMouseHoveringRect(at, ImVec2(at.x + width, at.y + box),
                                               false)) {
            const float size = em(1.6F);
            ImGui::SetCursorScreenPos(ImVec2(at.x + width - size - em(0.2F),
                                             at.y + em(0.2F)));
            const IconHit slot = icon_slot("##stop-cook", size);
            theme::draw_stop(draw, slot.center, em(0.9F),
                             slot.hovered ? theme::kFlameBright : theme::kTextFaint);
            ImGui::SetItemTooltip("Stop now, without the finishing pass");
            if (slot.clicked) {
                stop_work();
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + box + em(0.5F)));
    }

    // --- where it is up to -------------------------------------------------
    //
    // "Pass" rather than "round": a cook is passes over the same goal, and the
    // word is what says the work is being gone over again rather than continued.
    {
        std::string clock = format_duration(cook.duration());
        if (cook.budget_seconds > 0 && cook.state == CookState::Working) {
            const long left = cook.budget_seconds - cook.duration().count();
            clock += left > 0
                ? "  \xC2\xB7  " + format_duration(std::chrono::seconds{left}) + " left"
                : "  \xC2\xB7  time up";
        }
        const bool live = cook.state == CookState::Working
                       || cook.state == CookState::Finishing;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float line = ImGui::GetTextLineHeight();
        const ImVec2 at  = ImGui::GetCursorScreenPos();
        theme::draw_dot(draw, ImVec2(at.x + em(0.3F), at.y + line * 0.5F), em(0.26F),
                        live ? theme::Dot::Active
                             : cook.state == CookState::Failed ? theme::Dot::Missing
                                                               : theme::Dot::Ready);
        ImGui::Dummy(ImVec2(em(0.85F), line));
        ImGui::SameLine(0.0F, 0.0F);
        ImGui::PushFont(theme::bold());
        text_colored(live ? theme::kFlameBright : theme::kTextDim, "%s",
                      std::string(cook_state_name(cook.state)).c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        text_colored(theme::kTextFaint, "  \xC2\xB7  pass %d  \xC2\xB7  %s",
                      cook.iterations, clock.c_str());
    }

    // Who has worked on it. A cook is not one expert: a HANDOFF sends the next
    // piece of work back through the delegator, so a long one may pass from a
    // programming expert to a writing one and back. The chain is the record of
    // that, and it is the same arrow the side menu draws, spelled out.
    const std::vector<ExpertId> experts = cook.experts_used();
    if (experts.size() > 1) {
        std::string names;
        for (std::size_t i = 0; i < experts.size(); ++i) {
            names += (i == 0 ? "" : "  \xE2\x86\x92  ")
                   + expert_label(config_.roster, experts[i]);
        }
        text_colored(theme::kFlameBright, "%s", names.c_str());
        ImGui::SetItemTooltip("Every expert this cook has been handed to, in order.");
    }

    if (cook.budget_seconds > 0) {
        const float done = std::clamp(
            static_cast<float>(cook.duration().count())
                / static_cast<float>(cook.budget_seconds), 0.0F, 1.0F);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme::to_vec(theme::kFlame));
        ImGui::ProgressBar(done, ImVec2(-FLT_MIN, em(0.25F)), "");
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, em(0.6F)));

    for (std::size_t i = 0; i < cook.steps.size(); ++i) {
        draw_cook_step(cook.steps[i], i);
    }

    // --- a question ---------------------------------------------------------
    //
    // The one thing on this screen that stops everything, so it is the one
    // thing drawn as an interruption rather than as another line of the log.
    if (cook.state == CookState::Asking && !cook.question.empty()) {
        ImGui::Dummy(ImVec2(0, em(0.7F)));
        const ImGuiStyle& style = ImGui::GetStyle();
        const float width = ImGui::GetContentRegionAvail().x;
        const float pad   = em(0.7F);
        const float wrap  = width - pad * 2.0F;
        const float tall  = ImGui::CalcTextSize(cook.question.c_str(),
                                                cook.question.c_str() + cook.question.size(),
                                                false, wrap).y;
        const ImVec2 at   = ImGui::GetCursorScreenPos();
        const float  line = ImGui::GetTextLineHeight();
        const float  box  = tall + pad * 2.0F + line * 2.0F + em(0.5F);
        ImDrawList*  draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(at, ImVec2(at.x + width, at.y + box),
                            IM_COL32(0xFF, 0x87, 0x00, 0x1E), style.ChildRounding);
        draw->AddRect(at, ImVec2(at.x + width, at.y + box), theme::kFlame,
                      style.ChildRounding, 0, 1.0F);

        ImGui::SetCursorScreenPos(ImVec2(at.x + pad, at.y + pad * 0.7F));
        ImGui::PushFont(theme::bold());
        text_colored(theme::kFlameBright, "It is asking");
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(at.x + pad, at.y + pad * 0.7F + line + em(0.25F)));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::to_vec(theme::kText));
        ImGui::TextUnformatted(cook.question.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::SetCursorScreenPos(ImVec2(at.x + pad, at.y + box - line - em(0.35F)));
        text_colored(theme::kTextFaint, "answer in the box below and press enter");
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + box + em(0.4F)));
    }

    if (!cook.outcome.empty()) {
        ImGui::Dummy(ImVec2(0, em(0.6F)));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, em(0.3F)));
        draw_markdown(cook.outcome, theme::kTextDim);
    }

    const std::vector<std::string> files = cook.files_touched();
    const bool finished = cook.state == CookState::Done || cook.state == CookState::Stopped
                       || cook.state == CookState::Failed;
    ImGui::Dummy(ImVec2(0, em(0.4F)));
    if (!files.empty()) {
        text_colored(theme::kTextFaint, "changed");
        for (const std::string& file : files) {
            text_colored(theme::kAdded, "  %s", file.c_str());
        }
    } else if (finished) {
        // The outcome above is the expert's account of itself; this is the
        // fact. A model that talked its way through an edit it never made
        // writes a confident summary of having made it, and the only thing that
        // catches that is the journal saying nothing was written.
        text_colored(theme::kError,
                      "changed no files -- whatever it says, nothing on disk moved");
    }
}

void App::draw_history() {
    title("History");
    wrapped(theme::kTextDim, "Everything Crucible has done in this project.");
    ImGui::Dummy(ImVec2(0, em(0.6F)));

    section("COOKS");
    const CookLog log(store_->project().dir);
    const std::vector<CookSummary> cooks = log.list();
    if (cooks.empty()) {
        wrapped(theme::kTextDim, "No cooks yet.");
    }
    for (const CookSummary& cook : cooks) {
        ImGui::Separator();
        wrapped(theme::kText, cook.goal);
        text_colored(theme::kTextDim, "%s  ·  %d %s  ·  %d steps  ·  %s  ·  %s",
                      cook.when().c_str(), cook.files,
                      cook.files == 1 ? "file" : "files", cook.steps,
                      format_duration(cook.duration).c_str(),
                      std::string(cook_state_name(cook.state)).c_str());
    }

    ImGui::Dummy(ImVec2(0, em(0.8F)));
    section("CONVERSATIONS");
    const std::vector<SessionSummary> sessions = store_->list();
    if (sessions.empty()) {
        wrapped(theme::kTextDim, "No conversations yet.");
    }
    for (const SessionSummary& session : sessions) {
        ImGui::Separator();
        wrapped(theme::kText, session.title);
        text_colored(theme::kTextDim, "%s  ·  %d turns", session.when().c_str(),
                      session.turns);
    }
}

/// The cook bar: a goal, how long to work, and the button that starts it.
///
/// Three states, because a cook has three. Before one starts this is the goal
/// box with its budget; while one runs it is the two ways to stop; and when the
/// cook has asked a question it is a box for the answer, which is the only
/// thing that will move it forward.
void App::draw_cook_composer(const Snapshot& snapshot) {
    const std::shared_ptr<const Cook> cook = snapshot.cook;
    const bool asking = cook && cook->state == CookState::Asking;

    ImGui::BeginChild("cook-composer", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);

    // The same column the transcript is set in -- see draw_chat_composer.
    const float room   = ImGui::GetContentRegionAvail().x;
    const float column = reading_column(room);
    if (column < room) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (room - column) * 0.5F);
    }

    if (asking) {
        const float button = em(5.0F);
        const float width  = std::max(column - button - ImGui::GetStyle().ItemSpacing.x,
                                      em(6.0F));
        const bool entered = grow_input("##cook-answer", "answer the question above",
                                        prompt_, width, kComposerLines,
                                        composer_input_height());
        ImGui::SameLine();
        const bool pressed = ImGui::Button("Answer", ImVec2(button, 0));
        if (entered || pressed) {
            submit_prompt();
            ImGui::SetKeyboardFocusHere(-1);
        }
    } else if (engine_->cooking()) {
        ImGui::PushFont(theme::bold());
        text_colored(theme::kFlameBright, "cooking");
        ImGui::PopFont();
        ImGui::SameLine();
        if (ImGui::Button("Stop and finish", ImVec2(em(9.0F), 0))) {
            // Not a cancel: it makes a finishing pass to leave the project in a
            // state that runs.
            engine_->stop_cook();
            say("wrapping up -- finishing touches, then it will stop");
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop now", ImVec2(em(5.6F), 0))) {
            engine_->cancel();
        }
        ImGui::SetItemTooltip("Stops immediately, without the finishing pass.");
    } else {
        // The goal, and the button that starts it. Nothing else.
        //
        // There was a minutes slider and a "No limit" checkbox here, and between
        // them they asked the user to commit to a number before they knew what
        // the work was -- while the checkbox that made the number meaningless sat
        // next to it. A cook now runs until it is finished or stopped, which is
        // what both of the buttons above are for.
        const float button = em(4.4F);
        const float width  = std::max(column - button - ImGui::GetStyle().ItemSpacing.x,
                                      em(6.0F));
        const bool entered = grow_input("##goal", "what should it work on?",
                                        cook_goal_, width, kComposerLines,
                                        composer_input_height());
        ImGui::SameLine();
        const bool pressed = ImGui::Button("Cook", ImVec2(button, 0));
        if (entered || pressed) {
            begin_cook();
        }
    }

    draw_usage_readout(snapshot, room, column);

    ImGui::EndChild();
}

}  // namespace crucible::gui
