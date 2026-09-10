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
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "../markdown_view.hpp"
#include "../theme.hpp"
#include "../widgets.hpp"
#include "crucible/util/diff.hpp"
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
    text_colored(theme::kFlameBright, "%s",
                  expert_label(roster, turn.route->expert).c_str());
    ImGui::PopFont();
    ImGui::SameLine();

    std::string rest = "  \xC2\xB7  "
                     + std::to_string(static_cast<int>(turn.route->confidence * 100.0F))
                     + "%  \xC2\xB7  " + std::string(route_source_name(turn.route->source));
    if (turn.load_ms > 0) {
        rest += "  \xC2\xB7  swapped in " + format::duration_ms(turn.load_ms);
    }
    text_colored(theme::kTextFaint, "%s", rest.c_str());
    ImGui::SetItemTooltip("How sure the delegator was, and what decided it.");
}

/// What can be done to one turn, drawn where the pointer already is.
///
/// A row of marks at the top right of the block, and only while the pointer is
/// somewhere inside it. Three, and each of them is something the transcript
/// could not do before: stop what is running, ask it again, throw it away.
///
/// Hidden until hovered because a transcript with three buttons beside every
/// exchange is a control panel with a conversation in it. Hovered, they are
/// exactly where the eye already is.
///
/// Returns which one was pressed. The caller acts on it *after* the loop over
/// the turns, because two of the three change how many turns there are.
enum class TurnControl { None, Stop, Retry, Delete };

TurnControl turn_controls(const ImVec2& block_min, const ImVec2& block_max,
                         bool running, bool busy) {
    // The hover test is the whole block, not the buttons: a control that only
    // appears once the pointer is already on top of it can never be found.
    if (!ImGui::IsMouseHoveringRect(block_min, block_max, false)) {
        return TurnControl::None;
    }

    // While something is running, the only honest offer is to stop it -- and
    // only on the turn that is actually running. Retrying or deleting a turn
    // mid-flight would renumber the thing the engine is writing into.
    struct Button { TurnControl action; const char* tip; };
    std::vector<Button> buttons;
    if (running) {
        buttons.push_back({TurnControl::Stop, "Stop"});
    } else if (!busy) {
        buttons.push_back({TurnControl::Retry, "Ask again"});
        buttons.push_back({TurnControl::Delete, "Delete"});
    }
    if (buttons.empty()) {
        return TurnControl::None;
    }

    const float size = em(1.6F);
    const ImVec2 keep = ImGui::GetCursorScreenPos();
    TurnControl  hit  = TurnControl::None;

    ImGui::PushID("turn-controls");
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        // Laid out from the right edge inwards, so the rightmost button is in
        // the same place whether there are one or two of them.
        const float x = block_max.x - size * static_cast<float>(buttons.size() - i);
        ImGui::SetCursorScreenPos(ImVec2(x, block_min.y));
        ImGui::PushID(static_cast<int>(i));
        const IconHit slot = icon_slot("##turn", size);
        const ImU32   ink  = slot.hovered ? theme::kFlameBright : theme::kTextFaint;
        switch (buttons[i].action) {
            case TurnControl::Stop:
                theme::draw_stop(ImGui::GetWindowDrawList(), slot.center, em(0.9F), ink);
                break;
            case TurnControl::Retry:
                theme::draw_retry(ImGui::GetWindowDrawList(), slot.center, em(0.95F), ink);
                break;
            case TurnControl::Delete:
                theme::draw_trash(ImGui::GetWindowDrawList(), slot.center, em(0.9F), ink);
                break;
            case TurnControl::None:
                break;
        }
        ImGui::SetItemTooltip("%s", buttons[i].tip);
        if (slot.clicked) {
            hit = buttons[i].action;
        }
        ImGui::PopID();
    }
    ImGui::PopID();

    // Put the cursor back. These are drawn out of the flow, over a block that
    // has already been laid out, and leaving the cursor where the last button
    // was would push everything after them sideways.
    ImGui::SetCursorScreenPos(keep);
    return hit;
}

}  // namespace

// ---------------------------------------------------------------------------
// The edit gate
// ---------------------------------------------------------------------------

void App::draw_pending_edit(const PendingEdit& edit) {
    const ImGuiStyle& style = ImGui::GetStyle();
    bool keep  = false;   // leave the file as it is
    bool apply = false;   // write the new one

    ImGui::Dummy(ImVec2(0, em(0.4F)));

    // --- a file that does not exist yet ------------------------------------
    //
    // One panel, not two. There is no "before" to compare against, and a column
    // headed "Now" saying "this file does not exist" is a space where a choice
    // should be -- it asks the reader to compare something with nothing. The
    // question here is not which of two files you want, it is whether this file
    // should exist at all, and that is a different question with two different
    // answers.
    if (edit.before.empty()) {
        ImGui::PushFont(theme::bold());
        text_colored(theme::kAdded, "New file");
        ImGui::PopFont();
        ImGui::SameLine();
        text_colored(theme::kText, "%s", edit.path.c_str());
        ImGui::Dummy(ImVec2(0, em(0.3F)));

        const RowMarks all{0, std::numeric_limits<std::size_t>::max(), '+'};
        draw_code_block(edit.after, edit.path, {}, 900, &all);

        ImGui::Dummy(ImVec2(0, em(0.4F)));
        const float button = em(7.0F);
        ImGui::PushStyleColor(ImGuiCol_Button, theme::to_vec(theme::kAddedWash));
        if (ImGui::Button("Allow", ImVec2(button, 0))) {
            apply = true;
        }
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Create %s with these contents.", edit.path.c_str());
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, theme::to_vec(theme::kRemovedWash));
        if (ImGui::Button("Deny", ImVec2(button, 0))) {
            keep = true;
        }
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Do not create the file.");

        ImGui::Dummy(ImVec2(0, em(0.4F)));
        if (keep) {
            engine_->approve_edit(false);
        } else if (apply) {
            engine_->approve_edit(true);
        }
        return;
    }

    // --- a file that already exists ----------------------------------------
    ImGui::PushFont(theme::bold());
    text_colored(theme::kFlameBright, "Change to a file");
    ImGui::PopFont();
    ImGui::SameLine();
    text_colored(theme::kText, "%s", edit.path.c_str());

    // Which lines actually moved, so the two columns can say so.
    //
    // Whole files side by side answer "which one do I want"; they are useless
    // at "what is different", and on a four-hundred-line file with one function
    // changed that is the only question anyone has. So the rows that differ are
    // washed -- red on the left for what goes, green on the right for what
    // arrives -- which is the diff's one good idea without giving up the two
    // files it is a diff of.
    const util::ChangedLines changed = util::changed_lines(edit.before, edit.after);
    const RowMarks removed{changed.first, changed.before_end, '-'};
    const RowMarks added  {changed.first, changed.after_end,  '+'};

    ImGui::SameLine();
    if (changed.identical()) {
        text_colored(theme::kTextFaint, "  \xC2\xB7  nothing would change");
    } else {
        text_colored(theme::kTextFaint, "  \xC2\xB7  %d removed, %d added",
                     static_cast<int>(changed.before_end - changed.first),
                     static_cast<int>(changed.after_end - changed.first));
    }
    ImGui::Dummy(ImVec2(0, em(0.3F)));

    if (ImGui::BeginTable("##edit", 2,
                          ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        struct Side {
            const char*        title;
            const char*        action;
            const std::string* body;
            ImU32              ink;
            const RowMarks*    marks;
            bool*              picked;
        };
        const Side sides[2] = {
            {"Now",      "Keep this", &edit.before, theme::kRemoved, &removed, &keep},
            {"Proposed", "Use this",  &edit.after,  theme::kAdded,   &added,   &apply},
        };

        ImGui::TableNextRow();
        for (int i = 0; i < 2; ++i) {
            ImGui::TableSetColumnIndex(i);
            ImGui::PushID(i);

            const float column = ImGui::GetContentRegionAvail().x;
            const float head   = ImGui::GetFrameHeight();
            const ImVec2 at    = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##pick", ImVec2(column, head));
            const bool hot = ImGui::IsItemHovered();
            if (ImGui::IsItemActivated()) {
                *sides[i].picked = true;
            }
            if (hot) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }

            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImU32 wash = (sides[i].ink & 0x00FFFFFFU) | (hot ? 0x55000000U : 0x22000000U);
            draw->AddRectFilled(at, ImVec2(at.x + column, at.y + head), wash,
                                style.ChildRounding);
            const float line = ImGui::GetTextLineHeight();
            draw->AddText(ImVec2(at.x + style.FramePadding.x, at.y + (head - line) * 0.5F),
                          sides[i].ink, sides[i].title);
            const float action_w = ImGui::CalcTextSize(sides[i].action).x;
            draw->AddText(ImVec2(at.x + column - action_w - style.FramePadding.x,
                                 at.y + (head - line) * 0.5F),
                          hot ? theme::kText : theme::kTextDim, sides[i].action);

            if (sides[i].body->empty()) {
                ImGui::Dummy(ImVec2(0, em(0.4F)));
                wrapped(theme::kTextFaint, "(empty)");
            } else {
                draw_code_block(*sides[i].body, edit.path, {}, 900 + i, sides[i].marks);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(0, em(0.4F)));

    if (keep) {
        engine_->approve_edit(false);
    } else if (apply) {
        engine_->approve_edit(true);
    }
}

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
    const auto  center = [&room](float item) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max((room - item) * 0.5F, 0.0F));
    };

    ImGui::Dummy(ImVec2(0, em(4.0F)));

    ImGui::PushFont(theme::heading());
    const ImVec2 size = ImGui::CalcTextSize(label);
    center(size.x);
    text_colored(fixable ? theme::kTextDim : theme::kTextFaint, "%s", label);
    ImGui::PopFont();

    // The button says Settings and nothing else. It is the second and last
    // thing on the screen, so what it does has to be readable from the two
    // words above it -- "No runtime / Settings" is a sentence; "No runtime /
    // Install a Vulkan or CUDA backend" is a paragraph pretending to be a
    // button.
    if (fixable) {
        ImGui::Dummy(ImVec2(0, em(0.9F)));
        const float width = em(8.0F);
        center(width);
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

    // What the hover buttons asked for, acted on after the loop: retry and
    // delete both change how many turns there are, and doing that underneath
    // the loop that is walking them is how a transcript ends up drawing a turn
    // that is no longer in it.
    TurnControl wanted     = TurnControl::None;
    std::size_t wanted_for = 0;

    for (std::size_t i = 0; i < snapshot.turns.size(); ++i) {
        const Turn& turn = snapshot.turns[i];
        // The ID a turn's code blocks hang off. Without it every block in the
        // transcript shares one state, and expanding a long one in the third
        // reply expands one in the first.
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 block_top = ImGui::GetCursorScreenPos();

        prompt_plate(turn.prompt);
        ImGui::Dummy(ImVec2(0, em(0.45F)));

        route_line(roster, turn);

        // What the turn did besides talk, before the answer that used it: a page
        // fetched, a file read or rewritten, a command run. Shown whether or not
        // the reply mentions them -- a model that edits one file and writes a
        // summary of editing another is not rare, and this is what catches it.
        for (std::size_t a = 0; a < turn.actions.size(); ++a) {
            const crucible::TurnAction& action = turn.actions[a];
            text_colored(theme::kTextDim, "   \xC2\xB7  %s", action.summary.c_str());
            // The diff it made or the output it printed, kept for good. This is
            // what used to vanish the moment the next round started.
            if (!action.body.empty()) {
                ImGui::Indent(em(1.2F));
                draw_code_block(action.body, action.language, action.language,
                                100 + static_cast<int>(a));
                ImGui::Unindent(em(1.2F));
            }
        }

        // The model's working, behind a disclosure triangle.
        //
        // It used to be all or nothing, decided once in Settings: on, every
        // reply carried a wall of the model talking to itself, and the answer
        // -- the thing that was asked for -- started a screen further down. Off,
        // a reasoning model that spent its whole budget thinking left no way to
        // find out what it had been thinking about.
        //
        // A triangle is the answer to both. The setting still decides whether a
        // turn opens expanded, so somebody who wants to watch still can; the
        // difference is that it is now a default rather than a verdict, and
        // either one can be changed on the turn in front of you.
        if (!turn.reasoning.empty()) {
            ImGui::Dummy(ImVec2(0, em(0.3F)));

            // Per turn, defaulting to whatever was last chosen anywhere.
            //
            // The two together are what makes this one control rather than two.
            // A turn you have explicitly folded stays folded even if you open a
            // later one; a turn you have not touched follows the last decision
            // you made -- so somebody who wants to watch every model think sets
            // that by opening one, not by going to look for a checkbox.
            ImGuiStorage* storage = ImGui::GetStateStorage();
            const ImGuiID key     = ImGui::GetID("thinking");
            const bool open = storage->GetBool(key, config_.ui.show_reasoning);

            const float  line = ImGui::GetTextLineHeight();
            const ImVec2 at   = ImGui::GetCursorScreenPos();
            const float  label_w = ImGui::CalcTextSize("thinking").x + em(1.4F);
            ImGui::InvisibleButton("##thinking", ImVec2(label_w, line + em(0.2F)));
            const bool hot = ImGui::IsItemHovered();
            if (ImGui::IsItemActivated()) {
                storage->SetBool(key, !open);
                // And it sticks. Written to the config rather than held for the
                // session, so it survives a restart and is the same preference
                // the terminal's /thinking toggles -- one setting, two faces,
                // and in the window it is reached by using it.
                if (!open != config_.ui.show_reasoning) {
                    update_config([open](Config& config) {
                        config.ui.show_reasoning = !open;
                    });
                }
            }
            if (hot) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            const ImU32 ink = hot ? theme::kTextDim : theme::kTextFaint;
            theme::draw_chevron(ImGui::GetWindowDrawList(),
                                ImVec2(at.x + em(0.35F), at.y + line * 0.55F),
                                line * 0.7F, ink, open);
            ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + em(0.9F), at.y), ink,
                                                "thinking");

            if (open) {
                ImGui::PushFont(theme::italic());
                wrapped(theme::kTextFaint, turn.reasoning);
                ImGui::PopFont();
            }
        }

        ImGui::Dummy(ImVec2(0, em(0.35F)));

        // Rendered, not printed. Every instruction-tuned model answers in
        // markdown whether or not you ask it to, and shown raw that is a wall
        // of asterisks with the structure left for the reader to reconstruct.
        draw_markdown(turn.reply, turn.failed ? theme::kError : theme::kText);

        if (turn.streaming && turn.reply.empty()) {
            text_colored(theme::kTextFaint, "...");
        }
        if (turn.canceled) {
            text_colored(theme::kTextFaint, "-- stopped");
        }

        if (turn.tokens_per_second > 0.0) {
            ImGui::Dummy(ImVec2(0, em(0.2F)));
            text_colored(theme::kTextFaint, "%s tok/s  \xC2\xB7  %d in  \xC2\xB7  %d out",
                          format::number(turn.tokens_per_second, 1).c_str(),
                          turn.prompt_tokens, turn.output_tokens);
        }
        ImGui::Dummy(ImVec2(0, em(0.7F)));

        // Drawn last, over the block it belongs to, because the block's bottom
        // edge is not known until it has been laid out.
        const ImVec2 block_bottom = ImGui::GetCursorScreenPos();
        if (const TurnControl action = turn_controls(
                ImVec2(block_top.x, block_top.y),
                ImVec2(block_top.x + ImGui::GetContentRegionAvail().x, block_bottom.y),
                turn.streaming, snapshot.busy);
            action != TurnControl::None) {
            wanted     = action;
            wanted_for = i;
        }

        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, em(0.5F)));
        ImGui::PopID();
    }

    switch (wanted) {
        case TurnControl::Stop:   stop_work();              break;
        case TurnControl::Retry:  retry_turn(wanted_for);   break;
        case TurnControl::Delete: delete_turn(wanted_for);  break;
        case TurnControl::None:   break;
    }

    // The gate, under everything, which is where the eye already is: it appears
    // in the middle of a turn that is streaming and the transcript is following
    // the bottom.
    if (snapshot.pending_edit) {
        draw_pending_edit(*snapshot.pending_edit);
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
    // Auto, then Send. Auto sits beside the box rather than in Settings because
    // whether you are watching an expert edit your files is a decision that
    // changes between one prompt and the next -- and a switch you have to go
    // and find is a switch that stays wherever it was last left.
    const float auto_w = ImGui::CalcTextSize("Auto").x
                       + ImGui::GetStyle().FramePadding.x * 2.6F;
    const float button = em(5.0F);
    const float width  = std::max(column - button - auto_w
                                      - ImGui::GetStyle().ItemSpacing.x * 2.0F,
                                  em(6.0F));
    const bool entered = grow_input("##prompt", hint, prompt_, width, kComposerLines,
                                    composer_input_height());

    ImGui::SameLine();
    {
        const bool on = config_.tools.auto_edits;
        ImGui::PushStyleColor(ImGuiCol_Button,
                              on ? theme::to_vec(theme::kFlame)
                                 : ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              on ? theme::to_vec(theme::kFlameBright)
                                 : ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              theme::to_vec(on ? theme::kInk : theme::kTextDim));
        if (ImGui::Button("Auto", ImVec2(auto_w, 0))) {
            update_config([on](Config& config) { config.tools.auto_edits = !on; });
        }
        ImGui::PopStyleColor(3);
        ImGui::SetItemTooltip(
            on ? "Auto is on: file edits are applied as the expert makes them.\n"
                 "Click to be asked about each one instead."
               : "Auto is off: every file edit stops and shows you the file as it "
                 "is\nbeside the file as it would be, and you pick one.\n"
                 "Click to apply edits without asking.\n\nCook always applies.");
    }

    ImGui::SameLine();
    const bool send = ImGui::Button(asking ? "Answer" : "Send", ImVec2(button, 0));
    if (entered || send) {
        submit_prompt();
        // Enter should leave the caret where it was, or every reply costs a
        // click to get back to typing.
        ImGui::SetKeyboardFocusHere(-1);
    }

    // --- what this conversation is costing ---------------------------------
    //
    // Under the box, because both numbers are about the conversation rather
    // than about the models -- which is why they are no longer in the side
    // menu, where they sat under a list of experts they had nothing to do with.
    //
    // The percentage is the one that changes behavior. Tokens in and out are a
    // running total that only goes up; how full the context is decides whether
    // the next turn quietly loses the start of this one, and it is the number
    // to watch before that happens rather than after.
    {
        const TokenUsage& usage = snapshot.session_usage;
        if (column < room) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (room - column) * 0.5F);
        }
        std::string line = format_tokens(usage.input_tokens) + " in  \xC2\xB7  "
                         + format_tokens(usage.output_tokens) + " out";
        if (snapshot.context_size > 0) {
            const int percent = std::min(
                100, static_cast<int>(std::lround(
                         100.0 * snapshot.context_used / snapshot.context_size)));
            line += "  \xC2\xB7  " + std::to_string(percent) + "% context used";
        }
        // Amber past three quarters. The prompt is allowed three quarters of
        // the window and the answer needs the rest, so that is the point at
        // which the next turn starts dropping things.
        const bool tight = snapshot.context_size > 0
                        && snapshot.context_used * 4 >= snapshot.context_size * 3;
        text_colored(tight ? theme::kFlameBright : theme::kTextFaint, "%s", line.c_str());
        ImGui::SetItemTooltip(
            snapshot.context_size > 0
                ? "Tokens this session, and how much of the expert's context the last "
                  "turn filled.\nPast three quarters, the oldest exchanges start being "
                  "dropped -- see Settings, Tools, Context."
                : "Tokens this session. The context readout appears once a turn has run.");
    }

    ImGui::EndChild();
}

}  // namespace crucible::gui
