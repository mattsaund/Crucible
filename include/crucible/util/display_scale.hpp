// SPDX-License-Identifier: MIT
//
// How large to draw the desktop app, from what the window system reports.
//
// Two numbers that platforms disagree about, and that the window once
// treated as one. On Windows and on Linux under X11 a window is measured in
// pixels, so a 150% display needs everything drawn at 1.5x. On a Mac -- and on
// Wayland -- a window is measured in points and the framebuffer behind it is
// twice as dense: the operating system has already done the scaling, and doing
// it again drew every font and every margin at twice its size.
#pragma once

namespace crucible::util {

struct DisplayScale {
    /// Window units per point of interface: what fonts and spacing are
    /// multiplied by. 1.5 on a 150% Windows display, 1 on any Mac.
    float layout = 1.0F;

    /// Framebuffer pixels per window unit: what text is rasterized at, so it is
    /// sharp without being bigger. 2 on a Retina Mac, 1 on Windows.
    float density = 1.0F;
};

/// From GLFW's content scale for the window (pixels per point) and the window's
/// width in window units and in framebuffer pixels.
DisplayScale display_scale(float content_scale, int window_width, int framebuffer_width);

struct WindowSize {
    int width  = 0;
    int height = 0;
};

/// The window to open on a monitor whose usable area is `work_width` by
/// `work_height` window units, at `layout` window units per point.
WindowSize default_window_size(int work_width, int work_height, float layout);

}  // namespace crucible::util
