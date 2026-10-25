// Viewpoint collision detail: which Context element stopped each ray, the
// per-element summary the panel lists, the blocked/open arcs the overlay
// draws, and the Context toggle the in-viewer picker uses. Pure functions
// over the study result; no viewer.

#include "SunStudy/ViewpointStudy.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace evp::sunstudy;

namespace {

const char* kNear = "{0D000000-0000-0000-0000-000000000001}";
const char* kFar = "{0D000000-0000-0000-0000-000000000002}";
const char* kIgnored = "{0D000000-0000-0000-0000-000000000003}";

// An axis-aligned box as twelve triangles.
geomsrv::Mesh Box (const std::string& guid, const double lo[3], const double hi[3])
{
    geomsrv::Mesh mesh;
    mesh.guid = guid;
    for (int corner = 0; corner < 8; ++corner)
        for (int axis = 0; axis < 3; ++axis)
            mesh.vertices.push_back ((corner >> axis) & 1 ? hi[axis] : lo[axis]);
    mesh.triangles = { 0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6, 0, 1, 5, 0, 5, 4,
                       2, 6, 7, 2, 7, 3, 0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5 };
    mesh.triMaterial.assign (12, 0);
    return mesh;
}

std::shared_ptr<geomsrv::Snapshot> Scene ()
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = 4242;
    const double nearLo[3] = { 4, -3, 0 }, nearHi[3] = { 5, 3, 4 };         // east, 4 m away
    const double farLo[3] = { -3, 8, 0 }, farHi[3] = { 3, 9, 4 };           // north, 8 m away
    const double ignoredLo[3] = { -9, -3, 0 }, ignoredHi[3] = { -8, 3, 4 }; // west, not Context
    snapshot->meshes = { Box (kNear, nearLo, nearHi), Box (kFar, farLo, farHi), Box (kIgnored, ignoredLo, ignoredHi) };
    return snapshot;
}

ViewpointOptions Eye ()
{
    ViewpointOptions options;
    options.point = { 0.0, 0.0, 1.6 };
    options.radius = 20.0;
    options.rays = 360;
    return options;
}

} // namespace

TEST (ViewpointCollisions, EveryRayNamesTheContextElementThatStoppedIt)
{
    const auto snapshot = Scene ();
    const auto result = RunViewpointStudy (snapshot, { kNear, kFar }, Eye ());
    ASSERT_TRUE (result.valid) << result.error;
    ASSERT_EQ (result.rayMesh.size (), result.perimeter.size ());
    size_t stopped = 0;
    for (size_t ray = 0; ray < result.rayMesh.size (); ++ray) {
        const auto& end = result.perimeter[ray];
        const double distance = std::hypot (end[0], end[1]);
        if (result.rayMesh[ray] < 0) {
            EXPECT_NEAR (distance, 20.0, 1e-9) << "open rays reach the radius";
            continue;
        }
        ++stopped;
        EXPECT_NE (result.rayMesh[ray], 2) << "a non-Context element never stops a ray";
        EXPECT_LT (distance, 20.0);
    }
    EXPECT_EQ (stopped, result.collisionCount);
    EXPECT_EQ (result.rayMesh[0], 0) << "due east is the near box";
    EXPECT_EQ (result.rayMesh[90], 1) << "due north is the far box";
    EXPECT_EQ (result.rayMesh[180], -1) << "due west passes the ignored box";

    const auto open = RunViewpointStudy (snapshot, {}, Eye ());
    ASSERT_TRUE (open.valid);
    EXPECT_TRUE (std::all_of (open.rayMesh.begin (), open.rayMesh.end (), [] (int64_t m) { return m == -1; }));
}

TEST (ViewpointCollisions, BlockersAreSummarisedMostRaysFirst)
{
    const auto snapshot = Scene ();
    const auto result = RunViewpointStudy (snapshot, { kFar, kNear }, Eye ());
    ASSERT_TRUE (result.valid) << result.error;
    const auto blockers = ViewpointBlockers (result, *snapshot);
    ASSERT_EQ (blockers.size (), 2u);
    EXPECT_EQ (blockers[0].guid, kNear) << "the near box subtends more rays";
    EXPECT_EQ (blockers[0].mesh, 0u);
    EXPECT_NEAR (blockers[0].nearest, 4.0, 1e-9);
    EXPECT_EQ (blockers[1].guid, kFar);
    EXPECT_NEAR (blockers[1].nearest, 8.0, 1e-9);
    EXPECT_GT (blockers[0].rays, blockers[1].rays);
    EXPECT_EQ (blockers[0].rays + blockers[1].rays, result.collisionCount);

    auto other = *snapshot;
    other.id = 1;
    EXPECT_TRUE (ViewpointBlockers (result, other).empty ()) << "a result for another snapshot";
    EXPECT_TRUE (ViewpointBlockers (ViewpointStudyResult (), *snapshot).empty ());
}

TEST (ViewpointCollisions, ArcsGroupConsecutiveRaysAndWrapAroundRayZero)
{
    EXPECT_TRUE (ViewpointArcs ({}).empty ());
    const auto whole = ViewpointArcs ({ -1, -1, -1, -1 });
    ASSERT_EQ (whole.size (), 1u);
    EXPECT_EQ (whole[0].count, 4u);
    EXPECT_EQ (whole[0].mesh, -1);

    // The blocked run 6,7,0,1 crosses ray 0 and is reported once.
    const auto arcs = ViewpointArcs ({ 3, 3, -1, -1, 5, -1, 3, 3 });
    ASSERT_EQ (arcs.size (), 4u);
    size_t total = 0;
    for (const auto& arc : arcs)
        total += arc.count;
    EXPECT_EQ (total, 8u);
    const auto wrapped = std::find_if (arcs.begin (), arcs.end (), [] (const ViewpointArc& a) { return a.mesh == 3; });
    ASSERT_NE (wrapped, arcs.end ());
    EXPECT_EQ (wrapped->first, 6u);
    EXPECT_EQ (wrapped->count, 4u);

    const auto study = RunViewpointStudy (Scene (), { kNear, kFar }, Eye ());
    ASSERT_TRUE (study.valid);
    size_t blocked = 0;
    for (const auto& arc : ViewpointArcs (study.rayMesh))
        blocked += arc.mesh >= 0 ? arc.count : 0;
    EXPECT_EQ (blocked, study.collisionCount);
}

TEST (ViewpointCollisions, ToggleAddsThenRemovesEverySpellingOfAnElement)
{
    std::vector<std::string> context { kFar };
    EXPECT_TRUE (ToggleViewpointContext (context, kNear));
    EXPECT_EQ (context, (std::vector<std::string> { kFar, kNear }));
    // The same element without braces and in lower case is the same element.
    context.push_back ("0d000000-0000-0000-0000-000000000001");
    EXPECT_FALSE (ToggleViewpointContext (context, kNear));
    EXPECT_EQ (context, (std::vector<std::string> { kFar }));
    EXPECT_FALSE (ToggleViewpointContext (context, kFar));
    EXPECT_TRUE (context.empty ());
}
