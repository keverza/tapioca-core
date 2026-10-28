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
    const GhPreviewDrawables drawables = BuildViewpointOverlay ({ 0, 0, 1.5 }, 10.0, nullptr, {});
    EXPECT_TRUE (drawables.depthTested.meshIndices.empty ());
    EXPECT_EQ (drawables.depthTested.lineVertices.size (), 180u * 6u);
    EXPECT_TRUE (drawables.xray.Empty ()) << "the isovist is occluded by the model, never x-ray";
}

TEST (ViewpointOverlay, TheFillIsAFanAboutTheResultPointAndBlockedEdgesTakeTheirColour)
{
    ViewpointOverlayStyle style;
    const auto result = Square (true);
    const GhPreviewDrawables drawables = BuildViewpointOverlay ({ 9, 9, 9 }, 10.0, &result, style);
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
    const GhPreviewDrawables drawables = BuildViewpointOverlay (result.options.point, 10.0, &result, style);
    for (const auto& vertex : drawables.depthTested.lineVertices)
        EXPECT_NE (vertex.rgba, style.blockedRgba);
}
