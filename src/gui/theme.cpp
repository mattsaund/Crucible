// SPDX-License-Identifier: MIT
//
// The palette, the ImGui style, the fonts, and the mark.
//
// Black, white and orange, with the orange being the same three values the
// terminal palette uses, so the two faces cannot drift into different oranges.
//
// Fonts: JetBrains Mono is embedded in the binary rather than looked for on the
// system, since a program that changes typeface depending on the machine is not
// one design. load_fonts asks for glyph ranges beyond Latin-1 -- punctuation,
// arrows, maths -- because ImGui's default range stops at 0x00FF and renders a
// bullet as a hollow box.
//
// The mark is drawn rather than loaded: a path filled twice, the core leaning
// against the body so the two edges cross instead of nesting, which is what
// makes it read as two tongues of one flame rather than a shape with an
// outline. It does not animate. Nothing here does.
#include "theme.hpp"

#include "fonts.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>

namespace crucible::gui::theme {

ImVec4 to_vec(ImU32 color) {
    return ImGui::ColorConvertU32ToFloat4(color);
}

void apply() {
    ImGuiStyle& style = ImGui::GetStyle();

    // Square-ish, not rounded. The terminal face is drawn from box-drawing
    // characters and has no curves in it at all; matching that keeps the two
    // recognizably the same program rather than a program and its friendlier
    // cousin.
    style.WindowRounding    = 0.0F;
    style.ChildRounding     = 2.0F;
    style.FrameRounding     = 2.0F;
    style.PopupRounding     = 2.0F;
    style.ScrollbarRounding = 2.0F;
    style.GrabRounding      = 2.0F;
    style.TabRounding       = 2.0F;

    style.WindowBorderSize = 0.0F;
    style.ChildBorderSize  = 1.0F;
    style.FrameBorderSize  = 1.0F;
    style.PopupBorderSize  = 1.0F;

    style.WindowPadding    = ImVec2(16, 14);
    style.FramePadding     = ImVec2(10, 7);
    style.ItemSpacing      = ImVec2(10, 8);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.ScrollbarSize    = 11.0F;
    style.GrabMinSize      = 11.0F;

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]        = to_vec(kInk);
    c[ImGuiCol_ChildBg]         = to_vec(kPanel);
    c[ImGuiCol_PopupBg]         = to_vec(kPanel);
    c[ImGuiCol_Border]          = to_vec(kPanelEdge);
    c[ImGuiCol_BorderShadow]    = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_Text]            = to_vec(kText);
    c[ImGuiCol_TextDisabled]    = to_vec(kTextFaint);

    c[ImGuiCol_FrameBg]         = to_vec(kRaised);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.16F, 0.16F, 0.18F, 1.0F);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.20F, 0.20F, 0.22F, 1.0F);

    c[ImGuiCol_TitleBg]         = to_vec(kPanel);
    c[ImGuiCol_TitleBgActive]   = to_vec(kPanel);
    c[ImGuiCol_TitleBgCollapsed]= to_vec(kPanel);
    c[ImGuiCol_MenuBarBg]       = to_vec(kPanel);

    c[ImGuiCol_ScrollbarBg]     = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]   = ImVec4(0.22F, 0.22F, 0.24F, 1.0F);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30F, 0.30F, 0.33F, 1.0F);
    c[ImGuiCol_ScrollbarGrabActive]  = to_vec(kFlame);

    // Orange is the only saturated color in the interface, so it is reserved
    // for the thing the eye should go to: what is selected, what is active,
    // what is running.
    c[ImGuiCol_CheckMark]       = to_vec(kFlame);
    c[ImGuiCol_SliderGrab]      = to_vec(kFlame);
    c[ImGuiCol_SliderGrabActive]= to_vec(kFlameBright);

    c[ImGuiCol_Button]          = to_vec(kRaised);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.24F, 0.24F, 0.26F, 1.0F);
    c[ImGuiCol_ButtonActive]    = to_vec(kFlame);

    c[ImGuiCol_Header]          = ImVec4(1.0F, 0.53F, 0.0F, 0.22F);
    c[ImGuiCol_HeaderHovered]   = ImVec4(1.0F, 0.53F, 0.0F, 0.32F);
    c[ImGuiCol_HeaderActive]    = ImVec4(1.0F, 0.53F, 0.0F, 0.45F);

    c[ImGuiCol_Separator]       = to_vec(kPanelEdge);
    c[ImGuiCol_SeparatorHovered]= to_vec(kFlame);
    c[ImGuiCol_SeparatorActive] = to_vec(kFlameBright);

    c[ImGuiCol_Tab]                = to_vec(kPanel);
    c[ImGuiCol_TabHovered]         = ImVec4(1.0F, 0.53F, 0.0F, 0.30F);
    c[ImGuiCol_TabSelected]        = to_vec(kRaised);
    c[ImGuiCol_TabDimmed]          = to_vec(kPanel);
    c[ImGuiCol_TabDimmedSelected]  = to_vec(kRaised);

    c[ImGuiCol_PlotLines]       = to_vec(kFlame);
    c[ImGuiCol_PlotHistogram]   = to_vec(kFlame);
    c[ImGuiCol_TextSelectedBg]  = ImVec4(1.0F, 0.53F, 0.0F, 0.30F);
    c[ImGuiCol_DragDropTarget]  = to_vec(kFlameBright);
    c[ImGuiCol_NavCursor]       = to_vec(kFlame);
}

namespace {

ImFont* g_body    = nullptr;
ImFont* g_bold    = nullptr;
ImFont* g_italic  = nullptr;
ImFont* g_heading = nullptr;

/// The first of `candidates` that exists and loads, or nullptr.
ImFont* first_available(const char* const* candidates, std::size_t count, float pixels,
                        const ImWchar* ranges, float density) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.GlyphRanges       = ranges;
    cfg.RasterizerDensity = density;
    for (std::size_t i = 0; i < count; ++i) {
        std::error_code ec;
        if (!std::filesystem::exists(candidates[i], ec)) {
            continue;
        }
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(candidates[i], pixels, &cfg)) {
            return font;
        }
    }
    return nullptr;
}

/// The flame outline, as one closed path.
///
/// A real flame silhouette rather than a leaf: it is asymmetric, the tip leans,
/// and the base curls inward to a notch. That notch is the whole difference
/// between something that reads as fire and something that reads as foliage,
/// and it is why this is a concave fill -- an earlier version was three stacked
/// convex teardrops, which had no notch and looked like a leaf with highlights.
///
/// It is narrow: a bounding box about four units wide to seven tall. An earlier
/// version bulged a long way out to the left and read as a plump teardrop at
/// icon sizes, where the silhouette is all there is. Taking the belly in is the
/// whole difference between a mark and a blob.
///
/// `lean` scales the sideways offset of the tip, so the core can lean the other
/// way from the body. Coordinates are fractions of `radius` about `base`, which
/// is the point the flame stands on.
///
/// packaging/crucible.svg is these same numbers. Reshape one and reshape both.
void flame_path(ImDrawList* draw, ImVec2 base, float radius, float lean) {
    const auto at = [&](float x, float y) {
        return ImVec2(base.x + x * radius, base.y + y * radius);
    };

    draw->PathClear();
    draw->PathLineTo(at(0.00F, 0.22F));                       // the notch
    // Down and out to the left foot. The notch is shallow on purpose: it rises
    // a fifth of the radius above the feet and no more. Cut deeper -- an
    // earlier version went to 0.14 -- and the two feet read as the arms of a
    // chevron rather than as the bottom of a flame, which at 16 pixels is the
    // only thing anyone sees.
    draw->PathBezierCubicCurveTo(at(-0.10F, 0.34F), at(-0.24F, 0.36F), at(-0.34F, 0.34F));
    // Up the left side in two sweeps, which is what gives it a waist. One long
    // curve from foot to tip bulges in the middle and looks like a leaf.
    draw->PathBezierCubicCurveTo(at(-0.46F, 0.10F), at(-0.42F, -0.18F), at(-0.28F, -0.44F));
    draw->PathBezierCubicCurveTo(at(-0.18F, -0.68F), at(-0.06F, -0.84F),
                                 at(0.14F * lean, -1.00F));   // the tip, leaning
    // And down the right, which is straighter -- a flame is not symmetric, and
    // making it so is the difference between fire and a teardrop.
    draw->PathBezierCubicCurveTo(at(0.24F, -0.76F), at(0.30F, -0.50F), at(0.31F, -0.24F));
    draw->PathBezierCubicCurveTo(at(0.32F, -0.02F), at(0.36F, 0.16F), at(0.34F, 0.34F));
    draw->PathBezierCubicCurveTo(at(0.24F, 0.40F), at(0.10F, 0.34F), at(0.00F, 0.22F));
}

}  // namespace

ImFont* body()    { return g_body; }
ImFont* bold()    { return g_bold != nullptr ? g_bold : g_body; }
ImFont* italic()  { return g_italic != nullptr ? g_italic : g_body; }
ImFont* heading() { return g_heading != nullptr ? g_heading : bold(); }

void load_fonts(float layout, float density) {
    ImGuiIO& io = ImGui::GetIO();
    // From nothing every time, so moving the window to a display with another
    // scale rebuilds the faces rather than piling new ones on the old.
    io.Fonts->Clear();
    g_body = g_bold = g_italic = g_heading = nullptr;
    io.FontGlobalScale = 1.0F;
    const float size = 15.0F * layout;

    // The glyphs to bake, beyond Latin.
    //
    // ImGui's default range is Latin-1 and nothing else, so the first bulleted
    // list rendered every "\xE2\x80\xA2" as a question mark. It is not only the
    // bullet: instruction-tuned models emit em dashes, curly quotes and
    // ellipses constantly, and every one of those would have been a "?" in an
    // answer that looked fine in the terminal.
    //
    // General punctuation and the arrow block cover all of it, and cost a few
    // hundred glyphs in an atlas that already holds two hundred and fifty.
    static const ImWchar kRanges[] = {
        0x0020, 0x00FF,  // Latin, Latin-1 supplement
        0x2010, 0x205E,  // dashes, quotes, bullet, ellipsis, dagger
        0x2190, 0x21FF,  // arrows
        0x2200, 0x22FF,  // maths operators, for a model showing its working
        0,
    };

#ifdef CRUCIBLE_HAS_EMBEDDED_FONT
    // Compiled in, so this cannot fail for want of a file. ImGui takes
    // ownership of a font buffer it is given and frees it on shutdown, which it
    // must not do to a static array -- FontDataOwnedByAtlas says so.
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    cfg.GlyphRanges          = kRanges;
    cfg.RasterizerDensity    = density;  // sharp on a Retina panel, not bigger

    const auto embedded = [&](const unsigned char* data, unsigned int bytes, float pixels) {
        return io.Fonts->AddFontFromMemoryTTF(
            const_cast<unsigned char*>(data), static_cast<int>(bytes), pixels, &cfg);
    };
    g_body    = embedded(fonts::kRegular, fonts::kRegular_size, size);
    g_bold    = embedded(fonts::kBold,    fonts::kBold_size,    size);
    g_italic  = embedded(fonts::kItalic,  fonts::kItalic_size,  size);
    // Headings are the same face a size and a half up. Markdown structure has
    // to be visible while scrolling past it, and weight alone does not do that
    // in a monospace family where every glyph is already the same width.
    g_heading = embedded(fonts::kBold,    fonts::kBold_size,    size * 1.32F);
#endif

    if (g_body == nullptr) {
        // No embedded font in this build. A system monospace keeps the
        // interface honest -- Crucible's other face is a terminal, and mixing a
        // proportional font in here would read as a different program.
        static const char* const kMono[] = {
            "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
            "/usr/share/fonts/truetype/hack/Hack-Regular.ttf",
            "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
            "/System/Library/Fonts/Menlo.ttc",
            "/System/Library/Fonts/SFNSMono.ttf",
            "C:/Windows/Fonts/consola.ttf",
        };
        g_body = first_available(kMono, IM_ARRAYSIZE(kMono), size, kRanges, density);
    }
    if (g_body == nullptr) {
        // The built-in is a bitmap face and looks it, but a window with ugly
        // text beats no window.
        g_body = io.Fonts->AddFontDefault();
        io.FontGlobalScale = layout;
    }
}

void draw_flame(ImDrawList* draw, ImVec2 center, float radius, float alpha) {
    const auto fade = [alpha](ImU32 color) {
        const ImU32 a = static_cast<ImU32>(((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
        return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    };

    // The same silhouette twice: the body, and a core two thirds the size
    // leaning the other way. Two shapes, because a flame is a shape with a
    // hotter middle and anything more is decoration.
    //
    // Nothing here moves. An earlier version flickered the whole mark with the
    // engine's pulse, which is the sort of thing that looks alive for a minute
    // and then makes a window impossible to sit next to.
    //
    // The offsets that center the silhouette on `center`. Measured, not
    // eyeballed: the drawn outline runs -0.411..0.346 across and -1.000..0.364
    // down about the point the flame stands on, so the foot goes half of that
    // vertical span below the center and the whole shape a little to the right
    // of it. Curves overshoot their control points, which is why this cannot be
    // read off the numbers in flame_path by eye -- and why the launcher icon is
    // fitted to a measured box rather than placed by hand.
    const ImVec2 base{center.x + radius * 0.03F, center.y + radius * 0.32F};
    flame_path(draw, base, radius, 1.0F);
    draw->PathFillConcave(fade(kFlame));

    // The core sits on the same foot and leans the other way, so the two edges
    // cross rather than nest -- which is what makes it read as two tongues of
    // one flame instead of a shape with an outline.
    flame_path(draw, ImVec2(base.x, base.y - radius * 0.03F), radius * 0.46F, -0.9F);
    draw->PathFillConcave(fade(kFlameBright));
}

void draw_dot(ImDrawList* draw, ImVec2 center, float radius, Dot dot) {
    // A diamond, not a circle: it is what the terminal panel uses, and the
    // shape is half of how a seat's state reads at a glance.
    const auto diamond = [&](float r, ImU32 color, bool filled) {
        const ImVec2 points[4] = {
            ImVec2(center.x,     center.y - r),
            ImVec2(center.x + r, center.y),
            ImVec2(center.x,     center.y + r),
            ImVec2(center.x - r, center.y),
        };
        if (filled) {
            draw->AddConvexPolyFilled(points, 4, color);
        } else {
            draw->AddPolyline(points, 4, color, ImDrawFlags_Closed, 1.6F);
        }
    };

    switch (dot) {
        case Dot::Active:
            diamond(radius, kFlameBright, true);
            break;
        case Dot::Loading:
            // A filled core inside a ring: loading, and the percentage beside
            // it is what actually moves. This used to breathe with a sine, and
            // a mark that pulses for the whole minute a 30B model takes to load
            // is exhausting to sit beside.
            diamond(radius * 0.55F, kFlame, true);
            diamond(radius, kFlame, false);
            break;
        case Dot::Ready:
            diamond(radius, kTextDim, false);
            break;
        case Dot::Missing:
            diamond(radius, kError, false);
            draw->AddLine(ImVec2(center.x - radius * 0.6F, center.y - radius * 0.6F),
                          ImVec2(center.x + radius * 0.6F, center.y + radius * 0.6F),
                          kError, 1.6F);
            break;
        case Dot::Empty:
            draw->AddCircleFilled(center, radius * 0.28F, kTextFaint);
            break;
    }
}

// ---------------------------------------------------------------------------
// The title bar's icons
// ---------------------------------------------------------------------------
//
// All four are built from rectangles, lines and one triangle, on the theory
// that an icon which needs a bezier to be recognizable is an icon that is
// trying to say too much at sixteen pixels. They are stroked at a width tied to
// `size` so they thicken with the display scale rather than fading to a hair on
// a 4K panel.

void draw_panel_icon(ImDrawList* draw, ImVec2 center, float size, ImU32 color,
                     bool open) {
    // A pane with a rail down its left edge -- the same picture the sidebar
    // actually is. Filled when the sidebar is open and hollow when it is not,
    // so the button shows the state rather than the action: a control that
    // shows the action has to be read twice, once for the arrow and once to
    // remember which way it was pointing last.
    const float half  = size * 0.5F;
    const float thick = std::max(1.0F, size * 0.09F);
    const ImVec2 lo{center.x - half, center.y - half * 0.82F};
    const ImVec2 hi{center.x + half, center.y + half * 0.82F};
    draw->AddRect(lo, hi, color, size * 0.14F, 0, thick);

    const float rail = lo.x + size * 0.34F;
    if (open) {
        draw->AddRectFilled(ImVec2(lo.x + thick, lo.y + thick),
                            ImVec2(rail, hi.y - thick), color, size * 0.10F,
                            ImDrawFlags_RoundCornersLeft);
    } else {
        draw->AddLine(ImVec2(rail, lo.y + thick), ImVec2(rail, hi.y - thick),
                      color, thick);
    }
}

void draw_gear(ImDrawList* draw, ImVec2 center, float size, ImU32 color) {
    // A cog: a solid body with square teeth standing off it, and a hole in the
    // middle.
    //
    // The first version of this was a thin ring with six spokes radiating out
    // of it, which is not a gear -- it is a sun, or an asterisk, and it read as
    // one at every size. What makes the difference is that the teeth are wide:
    // a tooth is a block about as broad as the gap beside it, so the outline
    // steps in and out. Drawn as radial lines they are hairs, and hairs around
    // a circle are rays.
    constexpr int   kTeeth = 8;
    constexpr float kTwoPi = 6.2831853F;
    const float body  = size * 0.34F;   // the solid disc
    const float tip   = size * 0.50F;   // how far the teeth reach
    const float half  = kTwoPi / static_cast<float>(kTeeth) * 0.26F;  // tooth half-width

    for (int i = 0; i < kTeeth; ++i) {
        const float angle = (static_cast<float>(i) / kTeeth) * kTwoPi;
        const auto at = [&](float a, float r) {
            return ImVec2(center.x + std::cos(a) * r, center.y + std::sin(a) * r);
        };
        // Slightly inside the body at the root, so the tooth and the disc
        // overlap and no seam shows between them.
        const ImVec2 points[4] = {
            at(angle - half, body * 0.92F),
            at(angle - half, tip),
            at(angle + half, tip),
            at(angle + half, body * 0.92F),
        };
        draw->AddConvexPolyFilled(points, 4, color);
    }
    // The body, as a stroked circle rather than a filled one, which is what
    // leaves the hole. Painting the ground back over the middle would work
    // until the button is hovered or lit, at which point the hole is the wrong
    // color -- an annulus does not have to know what is behind it.
    const float hole = size * 0.15F;
    draw->AddCircle(center, (body + hole) * 0.5F, color, 28, body - hole);
}

void draw_copy(ImDrawList* draw, ImVec2 center, float size, ImU32 color) {
    const float half  = size * 0.5F;
    const float thick = std::max(1.0F, size * 0.09F);
    const float sheet = size * 0.62F;
    // The back sheet, then the front one over it, offset down and right.
    draw->AddRect(ImVec2(center.x - half, center.y - half),
                  ImVec2(center.x - half + sheet, center.y - half + sheet),
                  color, size * 0.12F, 0, thick);
    draw->AddRectFilled(ImVec2(center.x + half - sheet - thick, center.y + half - sheet - thick),
                        ImVec2(center.x + half + thick, center.y + half + thick),
                        kRaised, size * 0.12F);
    draw->AddRect(ImVec2(center.x + half - sheet, center.y + half - sheet),
                  ImVec2(center.x + half, center.y + half),
                  color, size * 0.12F, 0, thick);
}

void draw_retry(ImDrawList* draw, ImVec2 center, float size, ImU32 color) {
    // An arc with an arrowhead on one end: the universal "go round again".
    // Three quarters of a circle, not a whole one -- a closed ring with a head
    // stuck on it reads as a clock, and the gap is what says the motion has a
    // beginning.
    constexpr float kTwoPi = 6.2831853F;
    const float radius = size * 0.38F;
    const float thick  = std::max(1.4F, size * 0.11F);
    const float from   = kTwoPi * 0.10F;
    const float to     = kTwoPi * 0.82F;

    draw->PathClear();
    draw->PathArcTo(center, radius, from, to, 24);
    draw->PathStroke(color, ImDrawFlags_None, thick);

    // The head, on the end the arc starts at, pointing the way round.
    const ImVec2 tip{center.x + std::cos(from) * radius,
                     center.y + std::sin(from) * radius};
    const float  head = size * 0.22F;
    const ImVec2 points[3] = {
        ImVec2(tip.x + head * 0.5F, tip.y - head * 0.2F),
        ImVec2(tip.x - head * 0.5F, tip.y - head * 0.5F),
        ImVec2(tip.x - head * 0.1F, tip.y + head * 0.6F),
    };
    draw->AddConvexPolyFilled(points, 3, color);
}

void draw_trash(ImDrawList* draw, ImVec2 center, float size, ImU32 color) {
    const float half  = size * 0.5F;
    const float thick = std::max(1.2F, size * 0.10F);
    const float lid   = center.y - half * 0.60F;

    // The lid, with the handle above it, then the bin under it.
    draw->AddLine(ImVec2(center.x - half * 0.86F, lid),
                  ImVec2(center.x + half * 0.86F, lid), color, thick);
    draw->AddLine(ImVec2(center.x - half * 0.34F, lid - size * 0.14F),
                  ImVec2(center.x + half * 0.34F, lid - size * 0.14F), color, thick);

    const float top    = lid + size * 0.10F;
    const float bottom = center.y + half * 0.78F;
    const float taper  = size * 0.08F;   // a bin is narrower at the bottom
    draw->AddLine(ImVec2(center.x - half * 0.66F, top),
                  ImVec2(center.x - half * 0.66F + taper, bottom), color, thick);
    draw->AddLine(ImVec2(center.x + half * 0.66F, top),
                  ImVec2(center.x + half * 0.66F - taper, bottom), color, thick);
    draw->AddLine(ImVec2(center.x - half * 0.66F + taper, bottom),
                  ImVec2(center.x + half * 0.66F - taper, bottom), color, thick);
}

void draw_stop(ImDrawList* draw, ImVec2 center, float size, ImU32 color) {
    // A filled square, which is what stop has meant on every transport control
    // since tape. Not an X: an X is "close this", and closing and stopping are
    // different enough that the wrong one is worth avoiding.
    const float half = size * 0.32F;
    draw->AddRectFilled(ImVec2(center.x - half, center.y - half),
                        ImVec2(center.x + half, center.y + half), color, size * 0.08F);
}

void draw_chevron(ImDrawList* draw, ImVec2 center, float size, ImU32 color,
                  bool open) {
    const float half = size * 0.34F;
    if (open) {
        const ImVec2 points[3] = {
            ImVec2(center.x - half, center.y - half * 0.6F),
            ImVec2(center.x + half, center.y - half * 0.6F),
            ImVec2(center.x,        center.y + half * 0.8F),
        };
        draw->AddConvexPolyFilled(points, 3, color);
        return;
    }
    const ImVec2 points[3] = {
        ImVec2(center.x - half * 0.6F, center.y - half),
        ImVec2(center.x - half * 0.6F, center.y + half),
        ImVec2(center.x + half * 0.8F, center.y),
    };
    draw->AddConvexPolyFilled(points, 3, color);
}

}  // namespace crucible::gui::theme
