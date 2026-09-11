// SPDX-License-Identifier: MIT
#include "crucible/util/display_scale.hpp"

#include <algorithm>

namespace crucible::util {

DisplayScale display_scale(float content_scale, int window_width, int framebuffer_width) {
    DisplayScale scale;
    if (window_width > 0 && framebuffer_width > 0) {
        scale.density = static_cast<float>(framebuffer_width) / static_cast<float>(window_width);
    }
    // Content scale is pixels per point and density is pixels per window unit,
    // so what is left once the framebuffer's share is taken out is window units
    // per point. A Mac reports 2 and 2, and the answer is 1; Windows at 150%
    // reports 1.5 and 1, and the answer is 1.5.
    if (content_scale > 0.0F) {
        scale.layout = content_scale / scale.density;
    }
    // Bounded, because a window system that reports nonsense -- zero, or a
    // framebuffer read before the window has one -- should cost a wrong size,
    // not an unreadable one.
    scale.layout  = std::clamp(scale.layout, 1.0F, 4.0F);
    scale.density = std::clamp(scale.density, 1.0F, 4.0F);
    return scale;
}

WindowSize default_window_size(int work_width, int work_height, float layout) {
    layout = std::max(layout, 1.0F);
    if (work_width <= 0 || work_height <= 0) {
        return {static_cast<int>(1280.0F * layout), static_cast<int>(820.0F * layout)};
    }
    // Decided in points, which is what the interface is drawn in, and only then
    // turned into window units. Deciding in window units gave a Mac a
    // 1120-point window and a 150% Windows laptop a 1120-pixel one, which is
    // 750 points and cramped.
    const float points_wide = static_cast<float>(work_width) / layout;
    const float points_high = static_cast<float>(work_height) / layout;
    // A share of the screen, within limits: comfortable on a laptop, not a wall
    // on a desktop -- and never bigger than the screen it opens on.
    const float wide =
        std::min(std::clamp(points_wide * 0.62F, 1120.0F, 2200.0F), points_wide * 0.95F);
    const float high =
        std::min(std::clamp(points_high * 0.70F, 760.0F, 1500.0F), points_high * 0.92F);
    return {static_cast<int>(wide * layout), static_cast<int>(high * layout)};
}

}  // namespace crucible::util
