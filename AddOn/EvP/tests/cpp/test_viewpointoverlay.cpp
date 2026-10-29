#include "ArchViz/ViewpointOverlayGeometry.hpp"

#include <gtest/gtest.h>

#include <cmath>

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

TEST (ViewpointOverlay, ASphereDrawsNoShellOnlyTheSurfacesItSeesLiftedTowardThePoint)
{
    ViewpointOverlayStyle style;
    evp::sunstudy::ViewpointStudyResult result;
    result.valid = true;
    result.options.mode = evp::sunstudy::ViewpointMode::Sphere;
    result.options.sphereRows = 2;
    result.options.sphereColumns = 4;
    result.options.point = { 0, 0, 0 };
    result.perimeter = { { 1, 0, -1 }, { 0, 1, -1 }, { -1, 0, -1 }, { 0, -1, -1 },
                         { 1, 0, 1 },  { 0, 1, 1 },  { -1, 0, 1 },  { 0, -1, 1 } };
    result.rayMesh = { -1, -1, -1, -1, 0, -1, -1, -1 };
    const GhPreviewDrawables bare = BuildViewpointOverlay (result.options, &result, style);
    EXPECT_TRUE (bare.depthTested.meshIndices.empty ()) << "no hull over the ray ends";

    // One seen triangle in the plane x = 2, wound so its normal points away.
    result.seenSurface = { { 2, 0, 0 }, { 2, 0, 1 }, { 2, 1, 0 } };
    const GhPreviewDrawables drawables = BuildViewpointOverlay (result.options, &result, style);
    const auto& bucket = drawables.depthTested;
    ASSERT_EQ (bucket.meshVertices.size (), 3u);
    EXPECT_EQ (bucket.meshIndices.size (), 3u);
    for (const auto& vertex : bucket.meshVertices) {
        EXPECT_EQ (vertex.rgba, style.fillRgba);
        EXPECT_NEAR (vertex.x, 2.0f - 0.015f, 1e-6f) << "lifted off the wall toward the point";
        EXPECT_LT (vertex.nx, -0.99f) << "facing the point";
    }
}

TEST (ViewpointOverlay, ASphericalIsovistDrawsTheViewSphereGlyphAtItsPoint)
{
    evp::sunstudy::ViewpointOptions options;
    options.mode = evp::sunstudy::ViewpointMode::Sphere;
    options.minElevationDegrees = -30.0;
    options.maxElevationDegrees = 60.0;
    const GhPreviewDrawables plan = BuildViewpointOverlay (evp::sunstudy::ViewpointOptions {}, nullptr, {});
    const GhPreviewDrawables sphere = BuildViewpointOverlay (options, nullptr, {});
    // Three rings of 48 for the glyph and two band rings, on top of the radius
    // guide's horizon and its own two band rings.
    EXPECT_GT (sphere.depthTested.lineVertices.size (), plan.depthTested.lineVertices.size () + 5u * 48u * 6u - 1u);
    bool nearPoint = false;
    for (const auto& vertex : sphere.depthTested.lineVertices)
        nearPoint |= std::abs (std::hypot (vertex.x - options.point[0], vertex.y - options.point[1],
                                           vertex.z - options.point[2]) -
                               kViewGlyphMetres) < 1e-4;
    EXPECT_TRUE (nearPoint) << "a ring 1 m round the point";
}

TEST (ViewpointOverlay, PointVisibilityGuidesAreDepthTestedLines)
{
    PointVisibilityGuide guide;
    guide.point[2] = 1.6;
    guide.hasTarget = true;
    guide.target[0] = 10.0;
    const GhPreviewDrawables cone = BuildPointVisibilityGuides (guide);
    EXPECT_TRUE (cone.xray.Empty ()) << "occluded by the model, not drawn through it";
    EXPECT_TRUE (cone.depthTested.meshIndices.empty ());
    // The target line, the ring and four rays.
    EXPECT_EQ (cone.depthTested.lineVertices.size (), (1u + 48u + 4u) * 6u);
    guide.sphere = true;
    const GhPreviewDrawables sphere = BuildPointVisibilityGuides (guide);
    EXPECT_EQ (sphere.depthTested.lineVertices.size (), 3u * 48u * 6u) << "a full band: the glyph's three rings";
}
