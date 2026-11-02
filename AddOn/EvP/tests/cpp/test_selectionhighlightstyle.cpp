// ArchViz/SelectionHighlightStyle -- how a selected element is marked (issue #118).
//
// These pin the RELATIONSHIPS between the marks, which are what the look depends on and what a
// casual retune breaks: hidden edges as strong as visible ones flatten the element; an outline
// back at the old 3 px is the cartoon outline; a selection that shares the hover's hue cannot be
// told from it.
//
// The picture itself has no offline renderer, so it is checked by eye in the standalone viewer
// (ArchVizPanel::OpenViewer), selecting in turn: a white wall, a dark material,
// a glazed curtain wall, the same wall in the sun study's white model, the same wall under the
// Wireframe and Shaded+Wireframe render modes, and a selected wall half hidden behind another
// one. Each must read as selected, keep its own material and shading, show its real edges (not
// its triangulation) bright where visible and faint where hidden, and on glass show an outline
// ring rather than a filled hull; hovering a selected element must leave it cyan, hovering an unselected one
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

TEST (SelectionHighlightStyle, OutlineIsSolidAndNotTheOldCartoonWeight)
{
    // Rejected live: a faint halo and a 0.70 core read as barely selected on a white model.
    EXPECT_FLOAT_EQ (kSelectionOutline.color[3], 1.0f);
    EXPECT_GE (kSelectionOutline.pixels, 1.5f);
    EXPECT_LT (kSelectionOutline.pixels, 3.0f) << "3 px was the original hard outline";
}

TEST (SelectionHighlightStyle, VisibleEdgesAreStrongAndHiddenEdgesFaint)
{
    EXPECT_FLOAT_EQ (kSelectionEdgeColor[3], 1.0f);
    EXPECT_GT (kSelectionHiddenEdgeColor[3], 0.15f) << "hidden edges must still be seen";
    EXPECT_LT (kSelectionHiddenEdgeColor[3], 0.5f) << "hidden edges must not read as visible";
    EXPECT_GT (kSelectionEdgePixels, kSelectionHiddenEdgePixels);
    EXPECT_GT (kSelectionHiddenEdgePixels, 0.0f);
}

TEST (SelectionHighlightStyle, EdgesShareTheOutlineHue)
{
    EXPECT_FLOAT_EQ (RgbDistance (kSelectionEdgeColor, kSelectionOutline.color), 0.0f);
    EXPECT_FLOAT_EQ (RgbDistance (kSelectionHiddenEdgeColor, kSelectionOutline.color), 0.0f);
}

TEST (SelectionHighlightStyle, SelectedEdgesOutweighTheGlobalWireframe)
{
    // Under the global wireframe the selection's edges are what tell its lines from the rest.
    EXPECT_GT (kSelectionEdgeColor[3], kWireframeColor[3]);
}

TEST (SelectionHighlightStyle, EdgeDepthPullIsASmallFractionOfDistance)
{
    EXPECT_GT (kSelectionEdgeDepthPull, 0.0f);
    EXPECT_LE (kSelectionEdgeDepthPull, 0.01f) << "a larger pull shows edges behind a face as visible";
}

TEST (SelectionHighlightStyle, HoverIsAnotherHueFromTheSelection)
{
    EXPECT_GT (RgbDistance (kHoverOutline.color, kSelectionOutline.color), 0.5f);
    for (const float* role : { kFocusOutlineColor, kAnalysisOutlineColor }) {
        EXPECT_GT (RgbDistance (role, kSelectionOutline.color), 0.3f);
        EXPECT_GT (RgbDistance (role, kHoverOutline.color), 0.3f);
    }
}

TEST (SelectionHighlightStyle, AnalysisRolesKeepTheirWeight)
{
    // The roles did not ask to get lighter with the selection.
    EXPECT_FLOAT_EQ (kAnalysisRoleOutlinePixels, 3.0f);
}

TEST (SelectionHighlightStyle, CreaseAngleDropsFacetsAndKeepsOctagons)
{
    // A 10-sided prism's facets meet at 36 degrees and merge; an octagon's at 45 and stay.
    EXPECT_GT (kSelectionCreaseDegrees, 36.0f);
    EXPECT_LT (kSelectionCreaseDegrees, 45.0f);
}
