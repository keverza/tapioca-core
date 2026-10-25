// Tests for SunStudy/SunStudyBounds -- the analysis cylinder.
//
// ⚠️ A WRONG SPLIT IS A PLAUSIBLE STUDY. An element dropped instead of turned
// into context stops casting shadow into the cylinder and its neighbour reads
// sunlit; so outside elements must come back as CONTEXT, never be left out.

#include <string>
#include <vector>

#include "Geometry/Mesh.hpp"
#include "SunStudy/SunStudyBounds.hpp"
#include "gtest/gtest.h"

using namespace evp::sunstudy;

namespace {

// A flat square of side 1 with its lower-left corner at (x, y), at height z.
geomsrv::Mesh Square (const std::string& guid, double x, double y, double z = 0.0)
{
    geomsrv::Mesh mesh;
    mesh.guid = guid;
    mesh.vertices = { x, y, z, x + 1, y, z, x + 1, y + 1, z + 3, x, y + 1, z };
    mesh.triangles = { 0, 1, 2, 0, 2, 3 };
    return mesh;
}

geomsrv::Snapshot Scene ()
{
    geomsrv::Snapshot snapshot;
    snapshot.meshes = { Square ("{AAAA-1}", 0, 0), Square ("{BBBB-2}", 10, 0), Square ("{CCCC-3}", 0, 20, -2) };
    return snapshot;
}

} // namespace

TEST (SunStudyBounds, BoundsCoverEveryVertex)
{
    double min[3], max[3];
    ASSERT_TRUE (SnapshotBounds (Scene (), min, max));
    EXPECT_DOUBLE_EQ (min[0], 0.0);
    EXPECT_DOUBLE_EQ (max[0], 11.0);
    EXPECT_DOUBLE_EQ (min[1], 0.0);
    EXPECT_DOUBLE_EQ (max[1], 21.0);
    EXPECT_DOUBLE_EQ (min[2], -2.0);
    EXPECT_DOUBLE_EQ (max[2], 3.0);
    EXPECT_FALSE (SnapshotBounds (geomsrv::Snapshot {}, min, max));
}

TEST (SunStudyBounds, OutsideElementsCastShadowAsContext)
{
    const BoundedRoles roles = SplitByCylinder (Scene (), { 0.5, 0.5, 2.0 }, {}, {});
    EXPECT_EQ (roles.analysis, (std::vector<std::string> { "{AAAA-1}" }));
    EXPECT_EQ (roles.context, (std::vector<std::string> { "{BBBB-2}", "{CCCC-3}" }));
}

TEST (SunStudyBounds, AFootprintReachingInIsMeasured)
{
    // Centre 1.5 m left of B's edge: B's box reaches into a 2 m circle.
    const BoundedRoles roles = SplitByCylinder (Scene (), { 8.5, 0.5, 2.0 }, {}, {});
    EXPECT_EQ (roles.analysis, (std::vector<std::string> { "{BBBB-2}" }));
}

TEST (SunStudyBounds, PickedContextStaysContextAndIgnoredIsInNeither)
{
    const BoundedRoles roles = SplitByCylinder (Scene (), { 5.0, 5.0, 100.0 }, { "{aaaa-1}" }, { "{CCCC-3}" });
    EXPECT_EQ (roles.analysis, (std::vector<std::string> { "{BBBB-2}" }));
    EXPECT_EQ (roles.context, (std::vector<std::string> { "{AAAA-1}" }));
}
