#include "ArchViz/ViewpointOverlayGeometry.hpp"

#include <gtest/gtest.h>

using namespace geomsrv::archviz;

namespace {

evp::sunstudy::ViewpointStudyResult Square (bool blocked)
{
    evp::sunstudy::ViewpointStudyResult result;
    result.valid = true;
    result.options.point = { 0, 0, 1.5 };
    result.perimeter = { { 2, 0, 1.5 }, { 0, 2, 1.5 }, { -2, 0, 1.5 }, { 0, -2, 1.5 } };
    result.rayMesh = { blocked ? 0 : -1, blocked ? 0 : -1, -1, -1 };
    return result;
}

} // namespace

TEST (ViewpointOverlay, WithoutAResultOnlyTheRadiusGuideIsBuilt)
{
    evp::sunstudy::ViewpointOptions options;
    options.radius = 10.0;
    const GhPreviewDrawables drawables = BuildViewpointOverlay (options, nullptr, {});
    EXPECT_TRUE (drawables.depthTested.meshIndices.empty ());
    EXPECT_EQ (drawables.depthTested.lineVertices.size (), 180u * 6u);
    EXPECT_TRUE (drawables.xray.Empty ()) << "the isovist is occluded by the model, never x-ray";
}

TEST (ViewpointOverlay, TheFillIsAFanAboutTheResultPointAndBlockedEdgesTakeTheirColour)
{
    ViewpointOverlayStyle style;
    const auto result = Square (true);
    evp::sunstudy::ViewpointOptions moved;
    moved.point = { 9, 9, 9 };
    const GhPreviewDrawables drawables = BuildViewpointOverlay (moved, &result, style);
    const auto& bucket = drawables.depthTested;
    ASSERT_EQ (bucket.meshVertices.size (), 5u);
    EXPECT_FLOAT_EQ (bucket.meshVertices[0].x, 0.0f) << "drawn where it was computed, not at the moved point";
    EXPECT_FLOAT_EQ (bucket.meshVertices[0].z, 1.5f);
    EXPECT_EQ (bucket.meshIndices.size (), 4u * 3u);
    for (const auto& vertex : bucket.meshVertices)
        EXPECT_EQ (vertex.rgba, style.fillRgba);
    size_t blocked = 0;
    for (const auto& vertex : bucket.lineVertices)
        blocked += vertex.rgba == style.blockedRgba ? 1 : 0;
    // One edge between the two stopped rays, plus a tick at each end of the run.
    EXPECT_EQ (blocked, 3u * 6u);
}

TEST (ViewpointOverlay, AnOpenIsovistHasNoBlockedMarks)
{
    ViewpointOverlayStyle style;
    const auto result = Square (false);
    const GhPreviewDrawables drawables = BuildViewpointOverlay (result.options, &result, style);
    for (const auto& vertex : drawables.depthTested.lineVertices)
        EXPECT_NE (vertex.rgba, style.blockedRgba);
}

TEST (ViewpointOverlay, AFieldOfViewFanStopsAtItsLastRayAndDrawsItsTwoEdges)
{
    ViewpointOverlayStyle style;
    evp::sunstudy::ViewpointStudyResult result;
    result.valid = true;
    result.options.mode = evp::sunstudy::ViewpointMode::FieldOfView;
    result.options.point = { 0, 0, 1.5 };
    result.perimeter = { { 2, 0, 1.5 }, { 1.4, 1.4, 1.5 }, { 0, 2, 1.5 } };
    result.rayMesh = { -1, -1, -1 };
    GhPreviewDrawables bare = BuildViewpointOverlay (result.options, nullptr, style);
    const GhPreviewDrawables drawables = BuildViewpointOverlay (result.options, &result, style);
    EXPECT_EQ (drawables.depthTested.meshIndices.size (), 2u * 3u) << "no triangle back from the last ray to the first";
    // Two perimeter edges and the two radial ones, beyond the guide.
    EXPECT_EQ (drawables.depthTested.lineVertices.size () - bare.depthTested.lineVertices.size (), 4u * 6u);
}

TEST (ViewpointOverlay, ASphereIsAHullOverItsRayEndsColouredByWhatStoppedThem)
{
    ViewpointOverlayStyle style;
    evp::sunstudy::ViewpointStudyResult result;
    result.valid = true;
    result.options.mode = evp::sunstudy::ViewpointMode::Sphere;
    result.options.sphereRows = 2;
    result.options.sphereColumns = 4;
    result.perimeter = { { 1, 0, -1 }, { 0, 1, -1 }, { -1, 0, -1 }, { 0, -1, -1 },
                         { 1, 0, 1 },  { 0, 1, 1 },  { -1, 0, 1 },  { 0, -1, 1 } };
    result.rayMesh = { -1, -1, -1, -1, 0, -1, -1, -1 };
    const GhPreviewDrawables drawables = BuildViewpointOverlay (result.options, &result, style);
    const auto& bucket = drawables.depthTested;
    ASSERT_EQ (bucket.meshVertices.size (), 8u);
    EXPECT_EQ (bucket.meshIndices.size (), 4u * 6u) << "one band of quads, wrapping all round";
    EXPECT_EQ (bucket.meshVertices[4].rgba & 0xFFFFFF00u, style.blockedRgba & 0xFFFFFF00u);
    EXPECT_EQ (bucket.meshVertices[5].rgba, style.fillRgba);
}
