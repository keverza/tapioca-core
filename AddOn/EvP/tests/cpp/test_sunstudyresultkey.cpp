// What a sun study's result depends on (SunStudy/SunStudyResultKey, #97/#98):
// a material edit keeps the study unless the Late preset's glass mask moved.

#include "SunStudy/SunStudyResultKey.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using evp::sunstudy::SunStudyGeometryKey;

namespace {

geomsrv::Mesh Quad (const std::string& guid, double x, int32_t material)
{
    geomsrv::Mesh mesh;
    mesh.guid = guid;
    mesh.vertices = { x, 0, 0, x + 1, 0, 0, x + 1, 1, 0, x, 1, 0 };
    mesh.triangles = { 0, 1, 2, 0, 2, 3 };
    mesh.triMaterial = { material, material };
    return mesh;
}

geomsrv::Snapshot Model ()
{
    geomsrv::Snapshot snapshot;
    snapshot.meshes = { Quad ("wall", 0, 1), Quad ("window", 5, 2) };
    snapshot.materialTransparency = { { 1, 0.0 }, { 2, 0.0 }, { 3, 0.8 }, { 4, 0.9 } };
    return snapshot;
}

} // namespace

// Live 2026-10-31 17:38: a material edit re-ran a 4.7 s refresh for 0 dirty samples.
TEST (SunStudyResultKey, AMaterialEditKeepsTheEarlyStudy)
{
    const auto before = Model ();
    auto after = before;
    after.id = before.id + 1;
    after.meshes[0].triMaterial = { 3, 3 }; // opaque -> glass
    EXPECT_EQ (SunStudyGeometryKey (before, false, 0.4), SunStudyGeometryKey (after, false, 0.4));
}

TEST (SunStudyResultKey, LateOnlyMovesWhenAFaceCrossesTheGlassThreshold)
{
    const auto before = Model ();
    auto opaque = before;
    opaque.meshes[0].triMaterial = { 2, 2 }; // opaque -> another opaque
    EXPECT_EQ (SunStudyGeometryKey (before, true, 0.4), SunStudyGeometryKey (opaque, true, 0.4));
    auto glassToGlass = before;
    glassToGlass.meshes[1].triMaterial = { 3, 3 };
    auto otherGlass = before;
    otherGlass.meshes[1].triMaterial = { 4, 4 };
    EXPECT_EQ (SunStudyGeometryKey (glassToGlass, true, 0.4), SunStudyGeometryKey (otherGlass, true, 0.4));
    auto oneFace = before;
    oneFace.meshes[0].triMaterial = { 1, 3 }; // one face becomes glass
    EXPECT_NE (SunStudyGeometryKey (before, true, 0.4), SunStudyGeometryKey (oneFace, true, 0.4));
    EXPECT_NE (SunStudyGeometryKey (before, true, 0.4), SunStudyGeometryKey (before, true, 0.95))
        << "the threshold is part of the mask";
}

TEST (SunStudyResultKey, GeometryMovesTheKeyAndMeshOrderDoesNot)
{
    const auto before = Model ();
    auto moved = before;
    moved.meshes[1].vertices[0] += 0.001;
    EXPECT_NE (SunStudyGeometryKey (before, false, 0.4), SunStudyGeometryKey (moved, false, 0.4));
    auto reordered = before;
    std::swap (reordered.meshes[0], reordered.meshes[1]); // an incremental snapshot lists updates first
    EXPECT_EQ (SunStudyGeometryKey (before, false, 0.4), SunStudyGeometryKey (reordered, false, 0.4));
    auto removed = before;
    removed.meshes.pop_back ();
    EXPECT_NE (SunStudyGeometryKey (before, false, 0.4), SunStudyGeometryKey (removed, false, 0.4));
}
