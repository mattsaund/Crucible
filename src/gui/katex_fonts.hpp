// SPDX-License-Identifier: MIT
//
// KaTeX's fonts, as bytes.
//
// The page draws math with KaTeX, whose stylesheet names twenty fonts by
// family, weight and style. The build embeds them from the KaTeX release
// pinned in cmake/CrucibleDependencies.cmake and writes this table beside
// them (build/generated/katex_fonts.cpp); window.cpp turns each row into an
// @font-face rule. Only compiled when the fetch succeeded --
// CRUCIBLE_HAS_KATEX is how the rest of the program finds out.
#pragma once

namespace crucible::gui::katex {

struct Face {
    const char*          family;   ///< "KaTeX_Main"
    int                  weight;   ///< 400 or 700
    bool                 italic;
    const unsigned char* data;     ///< the woff2 file
    unsigned int         size;
};

extern const Face         kFaces[];
extern const unsigned int kFaceCount;

}  // namespace crucible::gui::katex
