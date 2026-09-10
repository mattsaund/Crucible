// SPDX-License-Identifier: MIT
// One place for every color Crucible draws with.
#pragma once

#include <ftxui/screen/color.hpp>

#include "crucible/engine/state.hpp"

namespace crucible::ui {

/// Crucible is black, white and orange, and nothing else.
///
/// Black is the terminal's own background -- never painted, so the program sits
/// in whatever the user already runs rather than stamping a rectangle over it.
/// White is every neutral: text, structure, anything the eye should read and
/// not linger on. Orange is heat, and heat means work: the fire, the seat that
/// is answering, the thing being pointed at. If it is orange, something is
/// happening.
///
/// These are 256-color palette indices rather than the sixteen named ones,
/// because there is no orange in the sixteen. Every terminal in use supports
/// 256 colors; the alternative is truecolor, which buys nothing here (the
/// palette holds the exact shades wanted) and costs `constexpr`, since FTXUI's
/// RGB constructors are not.
namespace theme {

// --- the fire -------------------------------------------------------------
// The crucible's own flame, which is the one thing on screen that is always
// moving. Banked when idle, brighter under load, and orange-red when the run
// failed -- the same three-step ramp the seats use, for the same reason.
inline constexpr ftxui::Color::Palette256 kFlame      = ftxui::Color::DarkOrange;  // 208
inline constexpr ftxui::Color::Palette256 kFlameBusy  = ftxui::Color::Orange1;     // 214
inline constexpr ftxui::Color::Palette256 kFlameError = ftxui::Color::OrangeRed1;  // 202

// --- seats ----------------------------------------------------------------
// A heat ramp, which is the one gradient this palette can express and happens
// to be exactly the right metaphor: cold gray for a seat with nothing in it,
// warming as a model is read in, hottest while it is answering.
//
// Two of these are close enough in hue that color alone would not separate
// them, and it does not have to. Every seat also carries a distinct glyph
// (◆ ◇ ◴ ✗ ·) and a loading seat carries a percentage, so the color reinforces
// a state that is already legible in monochrome -- which is the only way a
// three-color palette can afford five states.
inline constexpr ftxui::Color::Palette256 kSeatActive       = ftxui::Color::Orange1;    // 214
inline constexpr ftxui::Color::Palette256 kSeatLoading      = ftxui::Color::DarkOrange; // 208
inline constexpr ftxui::Color::Palette256 kSeatDormant      = ftxui::Color::Grey62;     // 247
inline constexpr ftxui::Color::Palette256 kSeatUnconfigured = ftxui::Color::Grey35;     // 240

// --- the transcript -------------------------------------------------------
// What the user typed is the brightest white on screen: it is the one thing
// they wrote, and it has to be findable while scrolling back through a long
// cook. Everything the machine says is a step down from it.
inline constexpr ftxui::Color::Palette256 kUser   = ftxui::Color::Grey100;    // 231
inline constexpr ftxui::Color::Palette256 kRoute  = ftxui::Color::DarkOrange; // 208
inline constexpr ftxui::Color::Palette256 kMeta   = ftxui::Color::Grey42;     // 242
// Notices are mostly informational -- /help, /models, startup device lines --
// so they take the calm color. Anything genuinely wrong uses kError.
inline constexpr ftxui::Color::Palette256 kNotice = ftxui::Color::Grey70;     // 249
inline constexpr ftxui::Color::Palette256 kError  = ftxui::Color::OrangeRed1; // 202
inline constexpr ftxui::Color::Palette256 kAccent = ftxui::Color::DarkOrange; // 208

// Panels that float above the transcript paint their own ground so the text
// underneath cannot show through. This is the only place black is drawn rather
// than inherited.
inline constexpr ftxui::Color::Palette256 kPanel     = ftxui::Color::Grey3;   // 232
inline constexpr ftxui::Color::Palette256 kPanelText = ftxui::Color::Grey85;  // 253

// The row under the cursor, in every list Crucible draws.
//
// A band rather than a color swap: `inverted` on an orange theme turns the
// selected row into a solid block of orange, which reads as an alert. A dark
// gray ground keeps the row's own colors and just lifts it off the page.
//
// kMeta is dim enough that secondary text -- a file size, a build date, a
// "(none)" -- would vanish against that band, so anything drawn on a
// highlighted row uses kMetaOnHighlight instead, and meta_color() below picks
// between the two.
inline constexpr ftxui::Color::Palette256 kHighlight       = ftxui::Color::Grey23; // 237
inline constexpr ftxui::Color::Palette256 kMetaOnHighlight = ftxui::Color::Grey70; // 249

// --- rendered markdown -----------------------------------------------------
// Models answer in markdown whether or not you ask them to. These are what its
// parts are drawn in; see util/markdown.hpp.
//
// Headings and list markers are structure, and structure is what orange is for
// here -- it is the skeleton of the answer, and being able to find it while
// scrolling is worth the ink. Code is white and slightly brighter than prose,
// because a code block is read character by character and dimming it makes it
// harder to do that.
inline constexpr ftxui::Color::Palette256 kHeading = ftxui::Color::DarkOrange; // 208
inline constexpr ftxui::Color::Palette256 kCode    = ftxui::Color::Grey74;     // 250
inline constexpr ftxui::Color::Palette256 kMarker  = ftxui::Color::Orange1;    // 214

// --- inside a code block ---------------------------------------------------
//
// The same relaxation the window makes, in the colors a terminal has. Chrome
// keeps the rule -- one saturated color, spent on what is running -- and a code
// block does not, because telling a string from a keyword from a comment is a
// job hue does and the one weight a terminal has was already spent on **bold**.
//
// Written as indices rather than names: these are chosen to sit near the
// window's own code palette, and the number is the only thing that says which
// gray or which purple. The hex each approximates is beside it.
inline constexpr auto kCodeText     = static_cast<ftxui::Color::Palette256>(252); // d0d0d0
inline constexpr auto kCodeKeyword  = static_cast<ftxui::Color::Palette256>(176); // d787d7
inline constexpr auto kCodeType     = static_cast<ftxui::Color::Palette256>(180); // d7af87
inline constexpr auto kCodeString   = static_cast<ftxui::Color::Palette256>(114); // 87d787
inline constexpr auto kCodeNumber   = static_cast<ftxui::Color::Palette256>(173); // d7875f
inline constexpr auto kCodeComment  = static_cast<ftxui::Color::Palette256>(243); // 767676
inline constexpr auto kCodeFunction = static_cast<ftxui::Color::Palette256>(75);  // 5fafff
inline constexpr auto kCodePunct    = static_cast<ftxui::Color::Palette256>(246); // 949494
inline constexpr auto kCodePreproc  = static_cast<ftxui::Color::Palette256>(73);  // 5fafaf
inline constexpr auto kCodeGutter   = static_cast<ftxui::Color::Palette256>(240); // 585858

// A diff's pair. The marker in the gutter is solid and the row behind it is
// washed; the code on the row keeps its own syntax colors either way, because
// an added line you cannot read is not much of an improvement on a removed one.
inline constexpr auto kDiffAdded     = static_cast<ftxui::Color::Palette256>(114); // 87d787
inline constexpr auto kDiffRemoved   = static_cast<ftxui::Color::Palette256>(174); // d78787
inline constexpr auto kDiffAddedBg   = static_cast<ftxui::Color::Palette256>(22);  // 005f00
inline constexpr auto kDiffRemovedBg = static_cast<ftxui::Color::Palette256>(52);  // 5f0000

}  // namespace theme

/// Secondary text, in the shade that stays readable on the row it is on.
ftxui::Color meta_color(bool highlighted);

/// The color the crucible's fire takes for a given mood.
ftxui::Color mood_color(Mood mood);

}  // namespace crucible::ui
