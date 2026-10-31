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

namespace {

// The wall of Quad(), re-emitted as one painted face does it (live 18:42:36):
// another vertex order, triangles swapped, each starting at another corner.
geomsrv::Mesh Reemitted (const geomsrv::Mesh& quad)
{
    geomsrv::Mesh mesh = quad;
    mesh.vertices = { quad.vertices[9], quad.vertices[10], quad.vertices[11],  // was 3
                      quad.vertices[0], quad.vertices[1],  quad.vertices[2],   // was 0
                      quad.vertices[6], quad.vertices[7],  quad.vertices[8],   // was 2
                      quad.vertices[3], quad.vertices[4],  quad.vertices[5] }; // was 1
    // old face 1 (0,2,3) first, from its second corner; then old face 0 (0,1,2).
    mesh.triangles = { 2, 0, 1, 1, 3, 2 };
    mesh.triMaterial = { 7, 1 };
    return mesh;
}

} // namespace

TEST (SunStudyResultKey, TheSameTrianglesInAnotherOrderKeepTheKey)
{
    const auto before = Model ();
    auto after = before;
    after.meshes[0] = Reemitted (before.meshes[0]);
    EXPECT_EQ (SunStudyGeometryKey (before, false, 0.4), SunStudyGeometryKey (after, false, 0.4));
    const auto keys0 = evp::sunstudy::MeshKeys (before, false, 0.4);
    const auto keys1 = evp::sunstudy::MeshKeys (after, false, 0.4);
    EXPECT_EQ (keys0[0].set, keys1[0].set);
    EXPECT_NE (keys0[0].order, keys1[0].order) << "the log can still say it was reordered";
}

TEST (SunStudyResultKey, FacesAreMatchedToTheirTwinsAndAChangedShapeIsRefused)
{
    const auto quad = Model ().meshes[0];
    const auto twin = evp::sunstudy::MatchFaces (quad, Reemitted (quad));
    EXPECT_EQ (twin, (std::vector<uint32_t> { 1, 0 }));
    auto flipped = Reemitted (quad);
    std::swap (flipped.triangles[0], flipped.triangles[1]); // the same corners, the other side out
    EXPECT_TRUE (evp::sunstudy::MatchFaces (quad, flipped).empty ()) << "winding is part of a face";
    auto moved = Reemitted (quad);
    moved.vertices[0] += 0.01;
    EXPECT_TRUE (evp::sunstudy::MatchFaces (quad, moved).empty ());
    auto fewer = quad;
    fewer.triangles.resize (3);
    EXPECT_TRUE (evp::sunstudy::MatchFaces (quad, fewer).empty ());
}
