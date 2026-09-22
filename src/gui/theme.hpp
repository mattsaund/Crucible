// SPDX-License-Identifier: MIT
//
// The desktop application's palette and its mark.
//
// The same three colors the terminal uses, in the same roles: black is the
// ground, white is everything neutral, orange is heat and heat means work. The
// terminal expresses that in 256-color indices because that is what a terminal
// has; here they are the exact RGB those indices name, so the two faces of
// Crucible are recognizably one program.
#pragma once

#include <imgui.h>

namespace crucible::gui::theme {

// The palette, as the 256-color indices the TUI uses actually render.
// DarkOrange is 208 (#ff8700), Orange1 is 214 (#ffaf00), OrangeRed1 is 202
// (#ff5f00). Keeping the numbers identical is what stops the two faces drifting
// into two different oranges.
constexpr ImU32 kFlame      = IM_COL32(0xFF, 0x87, 0x00, 0xFF);
constexpr ImU32 kFlameBright = IM_COL32(0xFF, 0xAF, 0x00, 0xFF);
constexpr ImU32 kError      = IM_COL32(0xFF, 0x5F, 0x00, 0xFF);

// Four steps of near-black, and they are four rather than one because the
// window has to say what is behind what: the ground, the panels on it, the
// line between them, and the controls that sit proud. The steps are small on
// purpose -- the room a dark interface has to work in is the top few percent
// of the range, and spending it on contrast between panels leaves none for the
// text. What used to be here was a step lighter at every level, which read as
// gray rather than as dark.
constexpr ImU32 kInk        = IM_COL32(0x08, 0x08, 0x0A, 0xFF);  ///< the ground
constexpr ImU32 kPanel      = IM_COL32(0x0E, 0x0E, 0x10, 0xFF);
constexpr ImU32 kPanelEdge  = IM_COL32(0x20, 0x20, 0x24, 0xFF);
constexpr ImU32 kRaised     = IM_COL32(0x16, 0x16, 0x1A, 0xFF);

constexpr ImU32 kText       = IM_COL32(0xEC, 0xEC, 0xEC, 0xFF);
constexpr ImU32 kTextDim    = IM_COL32(0x8C, 0x8C, 0x92, 0xFF);
constexpr ImU32 kTextFaint  = IM_COL32(0x5A, 0x5A, 0x60, 0xFF);

// --- inside a code block ---------------------------------------------------
//
// Everything above is chrome, and chrome keeps the rule: one saturated color,
// spent on what is running. A code block is not chrome. It is the thing the
// reader came for, it is read a token at a time rather than glanced at, and
// telling a string from a keyword from a comment is a job that hue does and
// weight cannot -- a monospace family has one weight of meaning to give and
// markdown already spent it on **bold**.
//
// So the relaxation is scoped: these colors appear between the borders of a
// code block and nowhere else, which is why the interface still reads as black,
// white and orange from across the room.
constexpr ImU32 kCodeText     = IM_COL32(0xD4, 0xD7, 0xDD, 0xFF);
constexpr ImU32 kCodeKeyword  = IM_COL32(0xC6, 0x78, 0xDD, 0xFF);
constexpr ImU32 kCodeType     = IM_COL32(0xE5, 0xC0, 0x7B, 0xFF);
constexpr ImU32 kCodeString   = IM_COL32(0x98, 0xC3, 0x79, 0xFF);
constexpr ImU32 kCodeNumber   = IM_COL32(0xD1, 0x9A, 0x66, 0xFF);
constexpr ImU32 kCodeComment  = IM_COL32(0x6A, 0x72, 0x80, 0xFF);
constexpr ImU32 kCodeFunction = IM_COL32(0x61, 0xAF, 0xEF, 0xFF);
constexpr ImU32 kCodePunct    = IM_COL32(0x99, 0xA0, 0xAC, 0xFF);
constexpr ImU32 kCodePreproc  = IM_COL32(0x56, 0xB6, 0xC2, 0xFF);

// A diff separates added from removed, and green and red are what every reader
// already knows. This used to be orange and gray, on the argument that the
// palette has one saturated color to spend -- which is right for the chrome
// and wrong here: added and removed are a *pair*, and a pair drawn in one hue
// and one absence of hue has to be read rather than seen. The tints are the
// weak half of each: the row is washed, the marker in the gutter is solid, and
// the code on the row keeps its own syntax colors either way.
constexpr ImU32 kAdded      = IM_COL32(0x7E, 0xC6, 0x99, 0xFF);
constexpr ImU32 kRemoved    = IM_COL32(0xE5, 0x84, 0x7B, 0xFF);
constexpr ImU32 kAddedWash  = IM_COL32(0x2E, 0x6B, 0x47, 0x3A);
constexpr ImU32 kRemovedWash= IM_COL32(0x7A, 0x33, 0x2C, 0x3A);

ImVec4 to_vec(ImU32 color);

/// Apply the whole style: colors, rounding, spacing.
void apply();

/// Load the interface faces.
///
/// JetBrains Mono, compiled into the binary -- see cmake/EmbedBinary.cmake for
/// why. One family for the whole interface, monospace throughout: Crucible's
/// other face is a terminal, and a desktop app in a proportional font beside a
/// TUI in a fixed one reads as two programs rather than two views of one.
///
/// Three weights, which is what rendering markdown needs: prose, **bold** and
/// *italic*. Falls back to a system monospace, and then to ImGui's built-in
/// bitmap face, because a missing typeface is not a reason to have no window.
///
/// `layout` is window units per point and sizes the faces; `density` is
/// framebuffer pixels per window unit and is what the glyphs are rasterized at,
/// so a Retina Mac gets sharp 15-point text rather than 30-point text. See
/// crucible/util/display_scale.hpp. Safe to call again when the display
/// changes: it clears the atlas and loads the faces afresh.
void load_fonts(float layout, float density);

/// The faces, after load_fonts. `body()` is never null; the others fall back to
/// it when only one face could be loaded.
ImFont* body();
ImFont* bold();
ImFont* italic();

/// The larger face used for markdown headings.
ImFont* heading();

/// The mark: a flame, drawn rather than loaded.
///
/// Vector shapes rather than an image, for three reasons. It is one shape, so a
/// file would be mostly overhead. It has to be crisp at sixteen pixels in a
/// title bar and at two hundred on a splash, which one bitmap cannot do. And an
/// asset would have to be found at runtime, which means an install layout and a
/// search path for a picture the program can simply draw.
///
/// One silhouette, filled twice: the body and a hotter core leaning the other
/// way. The base curls inward to a notch, which is the whole difference between
/// something that reads as fire and something that reads as a leaf.
///
/// It does not move. Nothing in this file does.
void draw_flame(ImDrawList* draw, ImVec2 center, float radius, float alpha = 1.0F);

/// A status diamond, the same vocabulary the terminal panel uses: filled
/// means working, hollow means ready, faint means nothing assigned. Static --
/// what moves is the percentage beside a loading seat, not the mark.
enum class Dot { Active, Loading, Ready, Missing, Empty };
void draw_dot(ImDrawList* draw, ImVec2 center, float radius, Dot dot);

/// The three drawn marks: the side-menu toggle, Settings, and Copy.
///
/// Three, and each is a control rather than a label. There was a folder next to
/// the project path too, which was decoration: the path already said it was a
/// folder, and a small drawn glyph beside it was one more thing to decode. It
/// says "Project:" now.
///
/// The interface is set in one monospace face, baked with Latin, punctuation,
/// arrows and maths and nothing else -- there is no gear at U+2699 in the atlas
/// and no hamburger anywhere in Unicode that is not a menu of three lines. So
/// the four marks a title bar needs are strokes on the draw list, which also
/// makes them sharp at any display scale instead of whatever size the glyph was
/// baked at.
///
/// `size` is the box each is drawn to fit, centered on `center`.
void draw_panel_icon(ImDrawList* draw, ImVec2 center, float size, ImU32 color,
                     bool open);
void draw_gear(ImDrawList* draw, ImVec2 center, float size, ImU32 color);
void draw_copy(ImDrawList* draw, ImVec2 center, float size, ImU32 color);

/// The three that appear on a turn when the pointer is over it: ask this again,
/// throw it away, and stop what is running.
void draw_retry(ImDrawList* draw, ImVec2 center, float size, ImU32 color);
void draw_trash(ImDrawList* draw, ImVec2 center, float size, ImU32 color);
void draw_stop(ImDrawList* draw, ImVec2 center, float size, ImU32 color);

/// A disclosure triangle, pointing down when `open` and right when not.
void draw_chevron(ImDrawList* draw, ImVec2 center, float size, ImU32 color,
                  bool open);

}  // namespace crucible::gui::theme
