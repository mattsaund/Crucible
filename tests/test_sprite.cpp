// SPDX-License-Identifier: MIT
//
// The flame.
//
// One property matters and it is not artistic: every frame the sprite produces
// is the same size. The expert panel lays the mark out beside a column of
// seats, and FTXUI sizes a vbox to its widest child -- so a frame one column
// narrow makes the whole panel step sideways for a few hundredths of a second,
// which reads as the mark twitching rather than burning.
//
// It is easy to break by hand: the frames are written out as text, and a
// trailing space that a stray editor eats is invisible in a diff.
#include "test_helpers.hpp"

#include "crucible/ui/widgets/flame_sprite.hpp"

namespace {

using crucible::Mood;
using crucible::ui::FlameSprite;

const std::vector<Mood>& all_moods() {
    static const std::vector<Mood> moods{Mood::Idle,     Mood::Routing, Mood::Loading,
                                         Mood::Thinking, Mood::Talking, Mood::Error};
    return moods;
}

/// Columns, not bytes: a multi-byte glyph is one column wide.
std::size_t columns(const std::string& line) {
    std::size_t width = 0;
    for (const char byte : line) {
        width += (static_cast<unsigned char>(byte) & 0xC0U) != 0x80U ? 1 : 0;
    }
    return width;
}

/// The line split into columns, so a braille cell can be compared as one glyph.
std::vector<std::string> glyphs(const std::string& line) {
    std::vector<std::string> out;
    for (const char byte : line) {
        if ((static_cast<unsigned char>(byte) & 0xC0U) != 0x80U) {
            out.emplace_back();
        }
        out.back().push_back(byte);
    }
    return out;
}

/// A column with nothing drawn in it. Braille pads with U+2800, the blank
/// braille cell, rather than with a space -- it is a glyph, not whitespace.
bool blank(const std::string& glyph) { return glyph == " " || glyph == "\u2800"; }

/// Twice the midpoint of the drawn part of a line, in columns, or -1 for an
/// empty line. Doubled so a flame two columns wide has an exact centre.
int twice_centre(const std::string& line) {
    const std::vector<std::string> cols = glyphs(line);
    int first = -1;
    int last  = -1;
    for (std::size_t i = 0; i < cols.size(); ++i) {
        if (!blank(cols[i])) {
            first = first < 0 ? static_cast<int>(i) : first;
            last  = static_cast<int>(i);
        }
    }
    return first < 0 ? -1 : first + last;
}

}  // namespace

TEST(every_frame_is_the_same_size) {
    for (const bool unicode : {false, true}) {
        for (const bool compact : {false, true}) {
            const FlameSprite  sprite(unicode);
            const std::size_t  rows = static_cast<std::size_t>(FlameSprite::height(compact));
            const std::size_t  cols = static_cast<std::size_t>(FlameSprite::width(compact));
            for (const Mood mood : all_moods()) {
                // Well past the longest flicker period, so both frames of every
                // mood are covered several times over.
                for (std::size_t tick = 0; tick < 64; ++tick) {
                    const std::vector<std::string> lines = sprite.render(mood, tick, compact);
                    CHECK_EQ(lines.size(), rows);
                    for (const std::string& line : lines) {
                        CHECK_EQ(columns(line), cols);
                    }
                }
            }
        }
    }
}

TEST(the_flicker_never_moves_the_foot) {
    // The two frames of a mood differ in the lean of the tip. If they differed
    // at the base as well, the mark would rock from side to side once a second
    // -- which is a far worse thing to sit next to than a still one.
    for (const bool unicode : {false, true}) {
        for (const bool compact : {false, true}) {
            const FlameSprite sprite(unicode);
            for (const Mood mood : all_moods()) {
                const std::string foot = sprite.render(mood, 0, compact).back();
                for (std::size_t tick = 0; tick < 64; ++tick) {
                    // Bound to a local: CHECK_EQ takes its arguments by const
                    // reference, and .back() on the temporary vector would leave
                    // one dangling.
                    const std::vector<std::string> lines = sprite.render(mood, tick, compact);
                    CHECK_EQ(lines.back(), foot);
                }
            }
        }
    }
}

TEST(the_flame_stands_on_the_bottom_row_and_stays_centred) {
    // Fire grows upward from where it is lit, and the plume grows and shrinks
    // about a fixed centre line rather than drifting across the column. Both
    // are what keep the mark still while the engine changes state -- the flame
    // gets shorter, it does not wander off.
    //
    // Within half a column of centre: the canvas is an odd number of columns
    // wide and the drawing is not symmetric, so an exact match is not a thing
    // that can be asked for.
    for (const bool unicode : {false, true}) {
        for (const bool compact : {false, true}) {
            const FlameSprite sprite(unicode);
            const int middle = FlameSprite::width(compact) - 1;  // doubled, as below
            for (const Mood mood : all_moods()) {
                for (std::size_t tick = 0; tick < 64; ++tick) {
                    const std::vector<std::string> lines = sprite.render(mood, tick, compact);
                    const int centre = twice_centre(lines.back());
                    CHECK(centre >= 0);
                    CHECK(centre >= middle - 1 && centre <= middle + 1);
                }
            }
        }
    }
}

TEST(the_flame_is_taller_the_harder_the_engine_is_working) {
    // The whole point of the sprite: a glance says whether anything is
    // happening. Measured as the first row with any ink in it, which is the
    // top of the flame.
    const FlameSprite sprite(false);
    const auto top = [&sprite](Mood mood) {
        const std::vector<std::string> lines = sprite.render(mood, 0, false);
        for (std::size_t row = 0; row < lines.size(); ++row) {
            if (lines[row].find_first_not_of(' ') != std::string::npos) {
                return row;
            }
        }
        return lines.size();
    };
    CHECK(top(Mood::Talking) < top(Mood::Loading));
    CHECK(top(Mood::Loading) < top(Mood::Idle));
}

TEST(nothing_is_drawn_out_of_slashes_any_more) {
    // The sprite used to be assembled by hand out of slashes and parentheses,
    // first as a crucible -- a pot on a stand -- and then as a flame of the
    // same construction. Both are gone: the frames are packaging/flame.txt
    // resampled, in braille where the terminal can take it and in hashes where
    // it cannot. These are the pieces a half-finished edit would leave behind.
    for (const bool unicode : {false, true}) {
        const FlameSprite sprite(unicode);
        for (const bool compact : {false, true}) {
            for (const Mood mood : all_moods()) {
                for (const std::string& line : sprite.render(mood, 0, compact)) {
                    for (const char* shard : {"/", "\\", "(", ")", "_", "-"}) {
                        CHECK(line.find(shard) == std::string::npos);
                    }
                }
            }
        }
    }
}

TEST(the_flame_is_the_one_from_packaging_flame_txt) {
    // Braille, at eight dots to a cell. Not a check that the drawing is right
    // -- no test can be -- but a check that it is still the braille drawing and
    // not something typed in over the top of it.
    const FlameSprite sprite(true);
    for (const bool compact : {false, true}) {
        for (const Mood mood : all_moods()) {
            for (const std::string& line : sprite.render(mood, 0, compact)) {
                for (const std::string& glyph : glyphs(line)) {
                    CHECK(glyph.size() == 3);
                    const auto code =
                        static_cast<unsigned>((static_cast<unsigned char>(glyph[0]) & 0x0FU) << 12U |
                                              (static_cast<unsigned char>(glyph[1]) & 0x3FU) << 6U |
                                              (static_cast<unsigned char>(glyph[2]) & 0x3FU));
                    CHECK(code >= 0x2800U && code <= 0x28FFU);
                }
            }
        }
    }
}

TEST(plain_ascii_is_available_for_terminals_that_need_it) {
    const FlameSprite ascii(false);
    for (const bool compact : {false, true}) {
        for (const Mood mood : all_moods()) {
            for (std::size_t tick = 0; tick < 64; ++tick) {
                for (const std::string& line : ascii.render(mood, tick, compact)) {
                    for (const char byte : line) {
                        CHECK(static_cast<unsigned char>(byte) < 0x80U);
                    }
                }
            }
        }
    }
}
