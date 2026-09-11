// SPDX-License-Identifier: MIT
//
// How big the desktop app draws itself, on each kind of display.
//
// None of this can be looked at from the machine these tests run on -- there
// is no Retina panel on a Linux CI runner -- which is exactly why the numbers
// are pinned here. The Mac is the case that shipped wrong: every font and
// margin at twice its size, because the scale macOS had already applied was
// applied again.
#include "test_helpers.hpp"

#include "crucible/util/display_scale.hpp"

using crucible::util::default_window_size;
using crucible::util::display_scale;
using crucible::util::DisplayScale;
using crucible::util::WindowSize;

TEST(a_retina_mac_draws_one_point_per_point_and_rasterizes_at_two) {
    // macOS: the window is 1120 points, its framebuffer 2240 pixels, and GLFW
    // reports a content scale of 2.
    const DisplayScale scale = display_scale(2.0F, 1120, 2240);
    CHECK_EQ(scale.layout, 1.0F);
    CHECK_EQ(scale.density, 2.0F);
}

TEST(a_windows_display_at_150_percent_draws_everything_at_one_and_a_half) {
    // Windows measures the window in pixels, so the framebuffer is the same size.
    const DisplayScale scale = display_scale(1.5F, 1680, 1680);
    CHECK_EQ(scale.layout, 1.5F);
    CHECK_EQ(scale.density, 1.0F);
}

TEST(an_ordinary_display_is_left_alone) {
    const DisplayScale scale = display_scale(1.0F, 2200, 2200);
    CHECK_EQ(scale.layout, 1.0F);
    CHECK_EQ(scale.density, 1.0F);
}

TEST(x11_at_double_dpi_scales_the_layout_and_wayland_scales_the_framebuffer) {
    // Two Linux desktops at the same 2x setting, getting there by opposite
    // routes: X11 the way Windows does, Wayland the way a Mac does.
    const DisplayScale x11 = display_scale(2.0F, 2200, 2200);
    CHECK_EQ(x11.layout, 2.0F);
    CHECK_EQ(x11.density, 1.0F);

    const DisplayScale wayland = display_scale(2.0F, 1100, 2200);
    CHECK_EQ(wayland.layout, 1.0F);
    CHECK_EQ(wayland.density, 2.0F);
}

TEST(a_window_system_that_reports_nothing_draws_at_one) {
    const DisplayScale scale = display_scale(0.0F, 0, 0);
    CHECK_EQ(scale.layout, 1.0F);
    CHECK_EQ(scale.density, 1.0F);
}

TEST(the_default_window_is_the_same_size_in_points_everywhere) {
    // A 14-inch MacBook's usable area, in points, drawn at 1.
    const WindowSize mac = default_window_size(1512, 944, 1.0F);
    CHECK_EQ(mac.width, 1120);
    CHECK_EQ(mac.height, 760);

    // A 1080p Windows laptop at 150%, in pixels: the same 1120 points wide,
    // which is 1680 pixels -- not 1120 pixels, which at that scale is 750
    // points and was the cramped window. Shorter than the minimum, because the
    // screen is.
    const WindowSize windows = default_window_size(1920, 1032, 1.5F);
    CHECK_EQ(windows.width, 1680);
    CHECK(windows.width <= 1920);
    CHECK(windows.height <= 1032);
}

TEST(the_default_window_neither_fills_a_big_desktop_nor_overflows_a_small_one) {
    const WindowSize big = default_window_size(3840, 2110, 1.0F);
    CHECK_EQ(big.width, 2200);
    CHECK(big.height <= 1500);

    const WindowSize tiny = default_window_size(1024, 700, 1.0F);
    CHECK(tiny.width <= 1024);
    CHECK(tiny.height <= 700);
}
