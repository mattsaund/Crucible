// SPDX-License-Identifier: MIT
//
// The small drawing helpers every panel shares.
//
// None of these know anything about Crucible; they are the vocabulary the
// panels are written in. They live here rather than in each panel so that a
// heading looks the same on every page -- which is most of what makes a window
// look like one program rather than six.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>

#include "crucible/cook/journal.hpp"
#include "crucible/engine/state.hpp"
#include "theme.hpp"

namespace crucible::gui {

/// How far a prompt box may grow before it starts scrolling instead. Past this
/// the box would be eating the conversation it belongs to.
constexpr int kComposerLines = 8;

/// Layout in multiples of the font size rather than in pixels.
///
/// The interface is loaded at the display's own scale, so a sidebar written as
/// 268 pixels is two thirds the width it should be on a 4K panel and the
/// composer under it gets clipped. Everything laid out here is in `em`, which
/// tracks whatever size the font was actually loaded at.
float em(float n);

/// Formatted text in one color.
void text_colored(ImU32 color, const char* fmt, ...) IM_FMTARGS(2);

/// Wrapped body text in one color.
void wrapped(ImU32 color, const std::string& text);

/// A section heading: small, faint, spaced above.
void section(const char* label);

/// A page title.
void title(const char* label);

/// How tall a grow_input holding `text` will be, so a caller laying out the
/// frame can reserve the room before the box is drawn.
float grow_input_height(const std::string& text, float width, int max_lines);

/// A text box that grows downward as its content wraps, rather than scrolling
/// sideways and hiding everything but the tail.
///
/// Enter submits and Ctrl+Enter starts a new line. That is the right way round
/// for a prompt box: sending is the common action, and a multi-line prompt is
/// the exception that can afford a modifier.
///
/// Returns true on submit. Grows to `max_lines` and then scrolls, because a box
/// that can grow without limit eventually leaves no room for the conversation
/// it belongs to.
///
/// `height` overrides that: pass a positive value to make the box exactly that
/// tall whatever is typed in it, which is what a composer the user has dragged
/// to a size of their own needs. Zero keeps the measured behavior.
bool grow_input(const char* id, const char* hint, std::string& text,
                float width, int max_lines, float height = 0.0F);

/// The width to set a transcript in, given the room there is for it.
///
/// A measure, not a fraction of the window. Prose set across a 2200-pixel
/// monitor runs to a hundred and fifty characters a line and the eye loses the
/// left margin coming back to it, which is why every book ever printed is
/// narrower than the table it sits on. Typographers put the comfortable measure
/// at 45-90 characters; code wants more than prose does, and a hundred and ten
/// is the width most projects already wrap their source at.
///
/// Counted in columns rather than in ems because the face is monospace and a
/// column is 0.6 of an em -- an "em" cap is nearly twice as wide as it reads,
/// which is a mistake that looks like the cap simply not working.
float reading_column(float available);

/// A square hit target for a drawn mark, with the hover plate behind it.
///
/// The caller draws the icon itself, because the four in theme.hpp take
/// different arguments and wrapping each in a std::function to hand it to a
/// button helper is more machinery than the button is. What this owns is the
/// part every icon button shares: the size, the hit test, the plate that
/// appears under the pointer, and where the center ended up.
struct IconHit {
    bool   clicked = false;
    bool   hovered = false;
    ImVec2 center;
};
IconHit icon_slot(const char* id, float size, bool lit = false);

/// One tab in the top bar: a word, and a bar under it when it is the one
/// showing. Not ImGui::Tab, which draws a folder tab with a filled body -- at
/// the top of a window that reads as a browser, and Crucible's three views are
/// three modes of one window rather than three documents in it.
bool top_tab(const char* label, bool selected, float height);

/// A model reference as it should be read.
///
/// The config stores a bare file name for a model in the models directory and
/// an absolute path for one anywhere else. The path is the useful thing to keep
/// and the useless thing to show: a combo box twenty ems wide renders
/// "/mnt/media_drive/.models/lmstudio-community/Qwen3-Cod" and stops, which
/// says nothing about which model it is. The full path is the tooltip.
std::string model_label(const std::string& reference);

/// A path trimmed from the left, so the end -- which is the part that says
/// which project this is -- survives.
std::string tail_of(const std::filesystem::path& path, std::size_t width);

/// The status mark for a seat, in the same vocabulary the terminal panel uses.
theme::Dot dot_for(SeatPhase phase);

/// A mood as one lowercase word, for the line under the mark.
const char* mood_text(Mood mood);

/// The color a cook step is drawn in: red for a failure, orange for a write,
/// flame for anything that ran, faint for the rest.
ImU32 step_color(const CookStep& step);

/// Subdirectories of `dir`, sorted, with hidden ones left out.
std::vector<std::filesystem::path> subdirectories(const std::filesystem::path& dir);

}  // namespace crucible::gui
