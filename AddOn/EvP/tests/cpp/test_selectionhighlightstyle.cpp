// ArchViz/SelectionHighlightStyle -- how a selected element is marked (issue #118).
//
// These pin the RELATIONSHIPS between the marks, which are what the look depends on and what a
// casual retune breaks: a halo that is not fainter than its core is a second hard outline; a core
// as heavy as the old 3 px is the cartoon outline back; a selection that shares the hover's hue
// cannot be told from it.
//
// The picture itself has no offline renderer, so it is checked by eye in the standalone viewer
// (ArchVizPanel::OpenViewer), selecting in turn: a white wall, a dark material,
// a glazed curtain wall, the same wall in the sun study's white model, the same wall under the
// Wireframe and Shaded+Wireframe render modes, and a selected wall half hidden behind another
// one. Each must read as selected, keep its own material and shading, and show no halo over the
// wall in front of it; hovering a selected element must leave it cyan, hovering an unselected one
// must draw amber.

#include <gtest/gtest.h>

#include "ArchViz/SelectionHighlightStyle.hpp"

#include <cmath>

using namespace geomsrv::archviz;

namespace {

// Largest per-channel difference in rgb: a crude but sufficient "is this another hue".
float RgbDistance (const float* a, const float* b)
{
    float d = 0.0f;
    for (int i = 0; i < 3; ++i)
        d = std::fmax (d, std::fabs (a[i] - b[i]));
    return d;
}

} // namespace

TEST (SelectionHighlightStyle, HaloIsWiderAndFainterThanTheCore)
{
    EXPECT_GT (kSelectionHalo.pixels, kSelectionCore.pixels);
    EXPECT_LT (kSelectionHalo.color[3], kSelectionCore.color[3]);
    EXPECT_LE (kSelectionHalo.color[3], 0.25f) << "a halo that is not faint is a second outline";
}

TEST (SelectionHighlightStyle, CoreIsThinAndNotOpaque)
{
    // The old look was a 3 px, fully opaque ring.
    EXPECT_GE (kSelectionCore.pixels, 1.0f);
    EXPECT_LE (kSelectionCore.pixels, 1.5f);
    EXPECT_LT (kSelectionCore.color[3], 1.0f);
}

TEST (SelectionHighlightStyle, HaloAndCoreShareOneHue)
{
    EXPECT_FLOAT_EQ (RgbDistance (kSelectionHalo.color, kSelectionCore.color), 0.0f);
}

TEST (SelectionHighlightStyle, SelectionWireframeIsFainterThanTheGlobalOne)
{
    EXPECT_LT (kSelectionWireColor[3], kWireframeColor[3]);
    EXPECT_LE (kSelectionWireColor[3], 0.35f) << "a strong wire on a dense element is a wash again";
    EXPECT_GT (kSelectionWirePixels, 0.0f);
}

TEST (SelectionHighlightStyle, HoverIsAnotherHueFromTheSelection)
{
    EXPECT_GT (RgbDistance (kHoverOutline.color, kSelectionCore.color), 0.5f);
    for (const float* role : { kFocusOutlineColor, kAnalysisOutlineColor }) {
        EXPECT_GT (RgbDistance (role, kSelectionCore.color), 0.3f);
        EXPECT_GT (RgbDistance (role, kHoverOutline.color), 0.3f);
    }
}

TEST (SelectionHighlightStyle, AnalysisRolesKeepTheirWeight)
{
    // The roles did not ask to get lighter with the selection.
    EXPECT_FLOAT_EQ (kAnalysisRoleOutlinePixels, 3.0f);
}
