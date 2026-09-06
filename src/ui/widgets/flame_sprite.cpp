// SPDX-License-Identifier: MIT
//
// The flame.
//
// The same drawing as packaging/flame.txt -- the mark the banner, both
// installers and the README print -- resampled onto a character grid small
// enough to sit beside the roster. It is braille: eight dots to a cell, which
// at nine cells across is an eighteen-dot canvas, and that is enough resolution
// for the curl of the tail to survive. Nothing here is drawn by hand.
//
// Whole frames per mood rather than a template with markers punched into it.
// What changes between moods is how much of the canvas the flame fills -- the
// full plume while tokens are coming out, banked to an ember at rest, gone out
// with smoke over a cold foot when something has failed -- and no amount of
// marker substitution expresses that. It is the point of the sprite: how hard
// it is burning is how hard the machine is working, so a glance at the corner
// of the screen answers "is it doing anything" without reading a word.
//
// Every frame is padded to the same width and height at render time, so the
// expert panel never reflows and the flame never slides sideways as it burns.
// The plume grows and shrinks about a fixed centre line, which is what keeps it
// from wandering across the column as the engine changes state.
#include "crucible/ui/widgets/flame_sprite.hpp"

#include <array>
#include <string_view>

#include "crucible/ui/theme.hpp"

namespace crucible::ui {
namespace {

using FullFrame    = std::array<const char*, 7>;
using CompactFrame = std::array<const char*, 5>;

// --- the flame, in braille -------------------------------------------------
//
// Two frames per mood. The flicker is a lean in the tip rather than a jump in
// the whole shape: a mark that leaps about is one you cannot sit next to, and
// the model load these states cover can run for a minute. Blank cells are
// U+2800, not spaces, so a line is exactly as many columns as it looks.

// Full plume. The only state that reaches the top row.
constexpr std::array<FullFrame, 2> kFullTalking{{
    {{R"(⠀⠀⠀⠸⣦⠀⠀⠀⠀)",
      R"(⠀⠀⠀⢰⣿⣷⡄⠀⠀)",
      R"(⠀⠀⣄⣿⠟⣿⣿⠀⠀)",
      R"(⠀⣰⣿⡟⢁⣿⣿⣿⡀)",
      R"(⣾⣿⡏⠀⠘⢹⣿⣿⠇)",
      R"(⢿⣿⠀⠀⠀⠀⢻⡯⠀)",
      R"(⠈⠛⠧⣀⠀⠠⠛⠁⠀)"}},
    {{R"(⠀⠀⠀⠀⢷⡄⠀⠀⠀)",
      R"(⠀⠀⠀⢀⣿⣷⣆⠀⠀)",
      R"(⠀⠀⢠⣸⡏⣿⣿⡄⠀)",
      R"(⠀⣠⣾⡟⢁⣿⣿⣿⡂)",
      R"(⣾⣿⡏⠀⠘⢹⣿⣿⠇)",
      R"(⢿⣿⠀⠀⠀⠀⢻⡯⠀)",
      R"(⠈⠛⠧⣀⠀⠠⠛⠁⠀)"}},
}};

// Routing, loading, thinking: burning steadily, two thirds of the canvas.
// Deliberately calm -- the status bubble underneath says which of the three it
// is, in words rather than in glyphs nobody can tell apart.
constexpr std::array<FullFrame, 2> kFullWorking{{
    {{R"(⠀⠀⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠈⣦⠀⠀⠀⠀)",
      R"(⠀⠀⠀⣰⣿⣷⠀⠀⠀)",
      R"(⠀⠀⣴⡿⢹⣿⣷⠀⠀)",
      R"(⠀⣿⣿⠀⠈⢻⡿⠀⠀)",
      R"(⠀⠈⠻⡄⠀⡼⠃⠀⠀)"}},
    {{R"(⠀⠀⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⣳⡀⠀⠀⠀)",
      R"(⠀⠀⠀⣠⡿⣿⡀⠀⠀)",
      R"(⠀⠀⣴⡿⢣⣿⣷⠀⠀)",
      R"(⠀⣿⣿⠀⠈⢻⡿⠀⠀)",
      R"(⠀⠈⠻⡄⠀⡼⠃⠀⠀)"}},
}};

// Banked, not out. An ember flicks off the tip every second or so -- enough
// movement that an idle flame still looks lit, and little enough that it is not
// asking to be watched.
constexpr std::array<FullFrame, 2> kFullIdle{{
    {{R"(⠀⠀⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⣵⡄⠀⠀⠀)",
      R"(⠀⠀⢀⣼⢫⣷⡀⠀⠀)",
      R"(⠀⠀⠘⢧⠀⠽⠀⠀⠀)"}},
    {{R"(⠀⠀⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠠⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⣸⡄⠀⠀⠀)",
      R"(⠀⠀⢀⣴⢫⣿⡀⠀⠀)",
      R"(⠀⠀⠘⢧⠀⠽⠀⠀⠀)"}},
}};

// Gone out: smoke drifting off a cold foot. The one state the sprite shows by
// taking something away rather than by adding movement.
constexpr std::array<FullFrame, 2> kFullError{{
    {{R"(⠀⠀⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠂⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠐⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠂⠀⠀⠀⠀)",
      R"(⠀⠀⠰⣄⠀⠖⠀⠀⠀)"}},
    {{R"(⠀⠀⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠈⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠁⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠈⠀⠀⠀⠀)",
      R"(⠀⠀⠰⣄⠀⠖⠀⠀⠀)"}},
}};

// --- the compact flame -----------------------------------------------------
//
// Seven cells by five, for the terminal that is too short for the full one.
// The same drawing at a smaller scale rather than a crop: a flame with its top
// sawn off reads as a bush.
constexpr std::array<CompactFrame, 2> kCompactTalking{{
    {{R"(⠀⠀⠈⣦⠀⠀⠀)",
      R"(⠀⠀⣰⣿⣷⠀⠀)",
      R"(⠀⣴⡿⢹⣿⣷⠀)",
      R"(⣿⣿⠀⠈⢻⡿⠀)",
      R"(⠈⠻⡄⠀⡼⠃⠀)"}},
    {{R"(⠀⠀⠀⣳⡀⠀⠀)",
      R"(⠀⠀⣠⡿⣿⡀⠀)",
      R"(⠀⣴⡿⢣⣿⣷⠀)",
      R"(⣿⣿⠀⠈⢻⡿⠀)",
      R"(⠈⠻⡄⠀⡼⠃⠀)"}},
}};

constexpr std::array<CompactFrame, 2> kCompactWorking{{
    {{R"(⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⡀⠀⠀⠀)",
      R"(⠀⠀⢠⣿⡆⠀⠀)",
      R"(⠀⣤⡟⠸⣿⠄⠀)",
      R"(⠀⠹⣄⠀⠟⠀⠀)"}},
    {{R"(⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⣄⠀⠀⠀)",
      R"(⠀⠀⢀⣿⣆⠀⠀)",
      R"(⠀⣤⡟⠹⣿⠄⠀)",
      R"(⠀⠹⣄⠀⠟⠀⠀)"}},
}};

constexpr std::array<CompactFrame, 2> kCompactIdle{{
    {{R"(⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⢀⣾⡆⠀⠀)",
      R"(⠀⠀⢿⠈⠟⠀⠀)"}},
    {{R"(⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⢀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⢀⣼⡆⠀⠀)",
      R"(⠀⠀⢿⠈⠟⠀⠀)"}},
}};

constexpr std::array<CompactFrame, 2> kCompactError{{
    {{R"(⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠄⠀⠀⠀)",
      R"(⠀⠀⠀⠠⠀⠀⠀)",
      R"(⠀⠰⣄⠀⠖⠀⠀)"}},
    {{R"(⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠀⠀⠀)",
      R"(⠀⠀⠀⠐⠀⠀⠀)",
      R"(⠀⠀⠀⠀⠂⠀⠀)",
      R"(⠀⠰⣄⠀⠖⠀⠀)"}},
}};

// --- the flame, in plain ASCII ---------------------------------------------
//
// For `ui.unicode = false`, and for the terminal whose font has no braille
// block -- where the drawing above comes out as a column of empty boxes, which
// is worse than a coarse mark. It is the same silhouette filled with hashes at
// one cell per pixel, so the shape carries even though the detail cannot: a
// character cell is twice as tall as it is wide, which leaves seven rows to say
// what twenty-eight dots said above.
//
// Two glyphs are not the flame and are not hashes: `'` is the ember the idle
// state flicks off, and `~` is smoke. Both are one column wide everywhere.
constexpr std::array<FullFrame, 2> kFullAsciiTalking{{
    {{R"(    #)",
      R"(    ###)",
      R"(   ####)",
      R"(  ######)",
      R"(#########)",
      R"(########)",
      R"(  #####)"}},
    {{R"(    ##)",
      R"(    ###)",
      R"(   #####)",
      R"(  ######)",
      R"(#########)",
      R"(########)",
      R"(  #####)"}},
}};

constexpr std::array<FullFrame, 2> kFullAsciiWorking{{
    {{"",
      "",
      R"(    #)",
      R"(   ###)",
      R"(  #####)",
      R"( #######)",
      R"(  #####)"}},
    {{"",
      "",
      R"(    ##)",
      R"(    ###)",
      R"(  ######)",
      R"( #######)",
      R"(  #####)"}},
}};

constexpr std::array<FullFrame, 2> kFullAsciiIdle{{
    {{"",
      "",
      "",
      "",
      R"(    #)",
      R"(  ####)",
      R"(  ####)"}},
    {{"",
      "",
      R"(    ')",
      "",
      R"(    #)",
      R"(  ####)",
      R"(  ####)"}},
}};

constexpr std::array<FullFrame, 2> kFullAsciiError{{
    {{"",
      "",
      R"(   ~)",
      "",
      R"(    ~)",
      "",
      R"(  ####)"}},
    {{"",
      R"(    ~)",
      "",
      R"(     ~)",
      "",
      "",
      R"(  ####)"}},
}};

constexpr std::array<CompactFrame, 2> kCompactAsciiTalking{{
    {{R"(   #)",
      R"(  ###)",
      R"( #####)",
      R"(#######)",
      R"( #####)"}},
    {{R"(   ##)",
      R"(   ###)",
      R"( ######)",
      R"(#######)",
      R"( #####)"}},
}};

constexpr std::array<CompactFrame, 2> kCompactAsciiWorking{{
    {{"",
      R"(   #)",
      R"(  ###)",
      R"( #####)",
      R"( ####)"}},
    {{"",
      R"(   ##)",
      R"(   ##)",
      R"( #####)",
      R"( ####)"}},
}};

constexpr std::array<CompactFrame, 2> kCompactAsciiIdle{{
    {{"",
      "",
      "",
      R"(   #)",
      R"(  ###)"}},
    {{"",
      R"(   ')",
      "",
      R"(   ##)",
      R"(  ###)"}},
}};

constexpr std::array<CompactFrame, 2> kCompactAsciiError{{
    {{R"(  ~)",
      "",
      R"(   ~)",
      "",
      R"( ####)"}},
    {{R"(   ~)",
      "",
      R"(  ~)",
      "",
      R"( ####)"}},
}};

/// How fast a mood flickers, in frames per alternation.
///
/// Talking is three times the rate of the waiting states because it is the one
/// moment the machine is actually producing, and it should be visibly different
/// from the waiting that surrounds it.
std::size_t period(Mood mood) {
    switch (mood) {
        case Mood::Talking: return 3;
        case Mood::Idle:    return 14;
        case Mood::Error:   return 10;
        default:            return 7;
    }
}

/// The frame to draw, as a pointer into the tables above.
///
/// Returned as a pointer rather than copied: these are constant for the life of
/// the program and a sprite is re-rendered every frame.
const char* const* frame_for(Mood mood, std::size_t tick, bool compact, bool unicode) {
    const std::size_t which = (tick / period(mood)) % 2;
    if (compact) {
        switch (mood) {
            case Mood::Idle:    return unicode ? kCompactIdle[which].data()
                                               : kCompactAsciiIdle[which].data();
            case Mood::Talking: return unicode ? kCompactTalking[which].data()
                                               : kCompactAsciiTalking[which].data();
            case Mood::Error:   return unicode ? kCompactError[which].data()
                                               : kCompactAsciiError[which].data();
            default:            return unicode ? kCompactWorking[which].data()
                                               : kCompactAsciiWorking[which].data();
        }
    }
    switch (mood) {
        case Mood::Idle:    return unicode ? kFullIdle[which].data() : kFullAsciiIdle[which].data();
        case Mood::Talking: return unicode ? kFullTalking[which].data()
                                           : kFullAsciiTalking[which].data();
        case Mood::Error:   return unicode ? kFullError[which].data()
                                           : kFullAsciiError[which].data();
        default:            return unicode ? kFullWorking[which].data()
                                           : kFullAsciiWorking[which].data();
    }
}

/// Columns, not bytes: a braille cell is three bytes and one column wide.
std::size_t columns(std::string_view line) {
    std::size_t width = 0;
    for (const char byte : line) {
        width += (static_cast<unsigned char>(byte) & 0xC0U) != 0x80U ? 1 : 0;
    }
    return width;
}

}  // namespace

FlameSprite::FlameSprite(bool unicode) : unicode_(unicode) {}

std::vector<std::string> FlameSprite::render(Mood mood, std::size_t tick, bool compact) const {
    const char* const* frame = frame_for(mood, tick, compact, unicode_);
    const std::size_t  rows  = static_cast<std::size_t>(height(compact));
    const std::size_t  cols  = static_cast<std::size_t>(width(compact));

    std::vector<std::string> lines;
    lines.reserve(rows);
    for (std::size_t r = 0; r < rows; ++r) {
        std::string line = frame[r];

        // Padded out to a constant width. FTXUI sizes a vbox to its widest
        // child, so ragged lines would make the panel breathe in and out by a
        // column as the flame changes height. The ASCII frames are written
        // without their trailing run of spaces, because a trailing space is
        // what an editor silently eats; the braille ones carry blank cells and
        // are already full width.
        for (std::size_t drawn = columns(line); drawn < cols; ++drawn) {
            line.push_back(' ');
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

std::string thought_bubble(Mood mood, const std::string& status, std::size_t tick) {
    if (mood == Mood::Idle && status.empty()) {
        return {};
    }

    std::string text = status.empty() ? std::string(mood_label(mood)) : status;

    // Trailing dots that march, so a long model load still looks alive even
    // when the percentage has not moved.
    //
    // Padded back out to a constant width. The bubble and the flame are centred
    // inside the same column, so a bubble that grows by a character shifts the
    // whole sprite half a column -- which reads as the mark rocking side to
    // side once a second, and is far more distracting than the dots are useful.
    if (mood != Mood::Idle && mood != Mood::Error) {
        constexpr std::size_t kMaxDots = 3;
        const std::size_t dots = (tick / 4) % (kMaxDots + 1);
        text += std::string(dots, '.');
        text += std::string(kMaxDots - dots, ' ');
    }
    return "( " + text + " )";
}

ftxui::Color meta_color(bool highlighted) {
    return highlighted ? ftxui::Color(theme::kMetaOnHighlight) : ftxui::Color(theme::kMeta);
}

ftxui::Color mood_color(Mood mood) {
    switch (mood) {
        case Mood::Error:   return theme::kFlameError;
        case Mood::Idle:    return theme::kFlame;
        case Mood::Routing:
        case Mood::Loading:
        case Mood::Thinking:
        case Mood::Talking: return theme::kFlameBusy;
    }
    return theme::kFlame;
}

}  // namespace crucible::ui
