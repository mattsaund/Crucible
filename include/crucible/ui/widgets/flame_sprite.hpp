// SPDX-License-Identifier: MIT
// The mark: a flame, drawn in text, burning at the height the engine is working
// at.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "crucible/engine/state.hpp"

namespace crucible::ui {

/// Renders Crucible's flame as fixed-width text lines.
///
/// Every frame is the same width and height for a given size, so the expert
/// panel never reflows as the fire moves. What changes is how much of the
/// canvas the flame fills: an ember at rest, a steady flame while a model is
/// being read in, the full plume while tokens are coming out, and smoke over a
/// cold foot when something has failed. That mapping is the point of the sprite
/// -- how hard it is burning is how hard the machine is working, so a glance at
/// the corner of the screen answers "is it doing anything" without reading a
/// word.
///
/// The drawing is packaging/flame.txt -- the mark the banner, both installers
/// and the README print -- resampled onto a character grid. Nothing about it is
/// drawn by hand, so the mark on screen is the mark on the tin.
class FlameSprite {
public:
    /// `unicode` picks the braille drawing, which is eight dots to a cell and
    /// fine enough to keep the curl of the flame's tail. False falls back to the
    /// same silhouette filled with hashes: coarse, but a terminal whose font has
    /// no braille block would otherwise show a column of empty boxes.
    explicit FlameSprite(bool unicode);

    /// `tick` is a free-running frame counter; `compact` selects the short
    /// five-row flame used when the terminal is too short for the full one.
    std::vector<std::string> render(Mood mood, std::size_t tick, bool compact) const;

    static int width(bool compact)  { return compact ? 7 : 9; }
    static int height(bool compact) { return compact ? 5 : 7; }

private:
    bool unicode_;
};

/// A one-line "status bubble" shown under the flame while it is working,
/// e.g. `( routing... )`. Returns an empty string when there is nothing to say.
std::string thought_bubble(Mood mood, const std::string& status, std::size_t tick);

}  // namespace crucible::ui
