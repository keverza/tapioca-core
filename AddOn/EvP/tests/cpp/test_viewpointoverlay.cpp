#include "ArchViz/ViewpointOverlayGeometry.hpp"
#include "ArchViz/AnalysisOverlayParams.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

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

TEST (AnalysisOverlayDepth, MeshesAreNeverPulledThroughModelOccluders)
{
    const auto params = AnalysisOverlayDrawParams (0.05f, 20000.0f, true);
    EXPECT_FALSE (params.pullMeshes) << "rasterizer bias must not carry an isovist in front of the model";
    EXPECT_GT (params.hiddenDashPixels, 0.0f) << "only guides have the intentional hidden dashed pass";
    EXPECT_FLOAT_EQ (params.surfaceDecalMaxMetres, 0.05f);
    EXPECT_GT (kGhPreviewSurfaceDecalDepthBiasNdc, 0.0f);
    EXPECT_LE (kGhPreviewSurfaceDecalDepthBiasNdc, 1.0f / 4194304.0f)
        << "surface decals get four D32 ULPs, never the old unbounded slope-scaled pull";
}

TEST (AnalysisOverlayDepth, DecalsPassCoplanarDepthButStayBehindForegroundWalls)
{
    constexpr float nearClip = 0.05f;
    constexpr float maxMetres = 0.05f;
    for (const bool perspective : { true, false }) {
        for (const float distance : { 50.0f, 200.0f, 500.0f }) {
            SCOPED_TRACE (std::string (perspective ? "perspective at " : "orthographic at ") +
                          std::to_string (distance) + " m");
            const float farClip = perspective ? 20000.0f : 2.0f * distance;
            const double projectionScale = double (farClip) / (double (farClip) - nearClip);
            // Double evaluates the cap itself; the float checks below separately
            // document where D32 can no longer resolve a 10 cm foreground wall.
            const auto depth = [&] (float viewDepth) -> double {
                return perspective ? projectionScale - nearClip * projectionScale / viewDepth
                                   : (viewDepth - nearClip) / (double (farClip) - nearClip);
            };
            const float clipW = perspective ? distance : 1.0f;
            const float bias = GhPreviewSurfaceDecalBiasNdc (clipW, nearClip, farClip, perspective, maxMetres);
            ASSERT_GT (bias, 0.0f);
            EXPECT_LE (bias, kGhPreviewSurfaceDecalDepthBiasNdc);
            const double decalDepth = depth (distance) - bias;
            EXPECT_LT (decalDepth, depth (distance)) << "the coplanar decal must win its own surface";
            EXPECT_GT (decalDepth, depth (distance - 0.1f)) << "a wall 10 cm in front must still occlude the decal";
            if (perspective && distance >= 200.0f)
                EXPECT_LT (bias, kGhPreviewSurfaceDecalDepthBiasNdc) << "the 5 cm cap must engage at distance";
            if (distance <= 200.0f) {
                const float decal32 = float (decalDepth);
                EXPECT_LT (decal32, float (depth (distance))) << "the offset must survive binary32 rounding";
                EXPECT_GT (decal32, float (depth (distance - 0.1f)));
            }
        }
    }
}

TEST (AnalysisOverlayDepth, PerspectiveCapUsesTheFragmentsOwnViewDepth)
{
    constexpr float nearClip = 0.05f;
    constexpr float farClip = 20000.0f;
    constexpr float maxMetres = 0.05f;
    // A long source triangle can span 100..2000 m. At an interior fragment at
    // 300 m the decal PS must use 300 m, not an interpolated vertex bias.
    const float bias = GhPreviewSurfaceDecalBiasNdc (300.0f, nearClip, farClip, true, maxMetres);
    const float expected = maxMetres * nearClip * (farClip / (farClip - nearClip)) / (300.0f * 300.0f);
    EXPECT_NEAR (bias, expected, 1.0e-12f);
    const double depth =
        double (farClip) / (farClip - nearClip) - nearClip * double (farClip) / (farClip - nearClip) / 300.0;
    const double wallDepth =
        double (farClip) / (farClip - nearClip) - nearClip * double (farClip) / (farClip - nearClip) / 299.9;
    EXPECT_GT (depth - bias, wallDepth) << "a wall 10 cm ahead still occludes at the interior fragment";
}

TEST (ViewpointOverlay, WithoutAResultOnlyTheRadiusGuideIsBuilt)
{
    evp::sunstudy::ViewpointOptions options;
    options.radius = 10.0;
    const GhPreviewDrawables drawables = BuildViewpointOverlay (options, nullptr, {});
    EXPECT_TRUE (drawables.depthTested.meshIndices.empty ());
    EXPECT_TRUE (drawables.surfaceDecals.Empty ());
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
    EXPECT_TRUE (drawables.surfaceDecals.Empty ()) << "plan and FOV fills keep the unbiased mesh pipeline";
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

TEST (ViewpointOverlay, ASphereDrawsOnlyUnmovedSeenSurfaceDecals)
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
    EXPECT_TRUE (bare.surfaceDecals.Empty ());

    // One seen triangle in the plane x = 2, wound so its normal points away.
    result.seenSurface = { { 2, 0, 0 }, { 2, 0, 1 }, { 2, 1, 0 } };
    const GhPreviewDrawables drawables = BuildViewpointOverlay (result.options, &result, style);
    EXPECT_TRUE (drawables.depthTested.meshIndices.empty ())
        << "seen patches never inherit the ordinary analysis mesh's depth policy";
    const auto& bucket = drawables.surfaceDecals;
    ASSERT_EQ (bucket.meshVertices.size (), 3u);
    EXPECT_EQ (bucket.meshIndices.size (), 3u);
    for (size_t i = 0; i < bucket.meshVertices.size (); ++i) {
        const auto& vertex = bucket.meshVertices[i];
        EXPECT_EQ (vertex.rgba, style.fillRgba);
        EXPECT_FLOAT_EQ (vertex.x, float (result.seenSurface[i][0]));
        EXPECT_FLOAT_EQ (vertex.y, float (result.seenSurface[i][1]));
        EXPECT_FLOAT_EQ (vertex.z, float (result.seenSurface[i][2]));
        EXPECT_LT (vertex.nx, -0.99f) << "facing the point without moving the surface silhouette";
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
        if (std::abs (
                std::hypot (vertex.x - options.point[0], vertex.y - options.point[1], vertex.z - options.point[2]) -
                kViewGlyphMetres) < 1e-4) {
            nearPoint = true;
            EXPECT_TRUE (vertex.rgba == kViewSphereGlyphRgba || vertex.rgba == kViewSphereBandRgba)
                << "blue, not the edge colour";
        }
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
