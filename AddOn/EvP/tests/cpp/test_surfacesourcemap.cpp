// Geometry/SurfaceSourceMap — correlating a picked face with extracted provenance.
//
// ⚠️ WHY OFFLINE: the helper is what stands between a live face pick and a
// claim about which source (body, polygon) the user selected. Getting it wrong
// does not fail -- it confidently names the WRONG face, which on a schedule or a
// takeoff is a wrong number rather than a visible fault. So it is pure, and
// every refusal (invalid, no match, ambiguous) is asserted here.
//
// ⚠️ THE AGGREGATES ARE NOT THE TEST. Plane, orientation, area and centroid
// alone cannot tell a unit square from a same-area, same-centroid diamond; the
// boundary comparison is what decides, and the diamond case pins that down.

#include "Geometry/Mesh.hpp"
#include "Geometry/SurfaceSourceMap.hpp"
#include "Geometry/SurfaceSourceBoundary.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

using geomsrv::Mesh;
using geomsrv::surfacemap::AnalyticArea;
using geomsrv::surfacemap::EnumerateSourceFaces;
using geomsrv::surfacemap::FaceQuery;
using geomsrv::surfacemap::MatchStatus;
using geomsrv::surfacemap::Resolve;
using geomsrv::surfacemap::SourceFace;

namespace {

// A z-plane square, two triangles. `reverse` winds it the other way (the same
// plane, the opposite sense).
Mesh Square (double x0, double y0, double z, int32_t body, int32_t pgon, bool reverse = false)
{
    Mesh m;
    const double x1 = x0 + 1.0, y1 = y0 + 1.0;
    m.vertices = { x0, y0, z, x1, y0, z, x1, y1, z, x0, y1, z };
    if (reverse)
        m.triangles = { 0, 2, 1, 0, 3, 2 };
    else
        m.triangles = { 0, 1, 2, 0, 2, 3 };
    m.triMaterial.assign (2, 0);
    m.triSourceBody.assign (2, body);
    m.triSourcePgon.assign (2, pgon);
    return m;
}

std::vector<double> SquarePoints (double x0, double y0, double z, bool reverse = false)
{
    const double x1 = x0 + 1.0, y1 = y0 + 1.0;
    if (reverse)
        return { x0, y0, z, x0, y1, z, x1, y1, z, x1, y0, z };
    return { x0, y0, z, x1, y0, z, x1, y1, z, x0, y1, z };
}

// A unit cube with SHARED vertices (the topology ModelerAPI hands out), one
// source polygon per face, so adjacent faces use the same welded vertex indexes.
Mesh SharedVertexCube ()
{
    Mesh m;
    m.vertices = {
        0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, // z = 0
        0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, // z = 1
    };
    m.triangles = {
        0, 3, 2, 0, 2, 1, // bottom (-Z)  pgon 1
        4, 5, 6, 4, 6, 7, // top    (+Z)  pgon 2
        0, 1, 5, 0, 5, 4, // front  (-Y)  pgon 3
        1, 2, 6, 1, 6, 5, // right  (+X)  pgon 4
        2, 3, 7, 2, 7, 6, // back   (+Y)  pgon 5
        3, 0, 4, 3, 4, 7, // left   (-X)  pgon 6
    };
    const int32_t facePgons[6] = { 1, 2, 3, 4, 5, 6 };
    m.triMaterial.assign (12, 0);
    m.triSourceBody.assign (12, 1);
    for (int face = 0; face < 6; ++face)
        for (int t = 0; t < 2; ++t)
            m.triSourcePgon.push_back (facePgons[face]);
    return m;
}

// One unit cell as two triangles, own vertices (coordinates, not indexes, are
// what the boundary comparison uses, so duplication is harmless).
void AddCell (Mesh& m, double x, double y)
{
    const uint32_t base = uint32_t (m.vertices.size () / 3);
    m.vertices.insert (m.vertices.end (), { x, y, 0.0, x + 1, y, 0.0, x + 1, y + 1, 0.0, x, y + 1, 0.0 });
    m.triangles.insert (m.triangles.end (), { base, base + 1, base + 2, base, base + 2, base + 3 });
    for (int t = 0; t < 2; ++t) {
        m.triMaterial.push_back (0);
        m.triSourceBody.push_back (1);
        m.triSourcePgon.push_back (1);
    }
}

// A 4 x 4 grid of unit cells with the central 2 x 2 removed: a convex-part
// decomposition of the square [0,4]^2 with a square hole [1,3]^2. Every piece is
// same-wound CCW, exactly as the extractor emits.
Mesh Ring ()
{
    Mesh m;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (!(i >= 1 && i <= 2 && j >= 1 && j <= 2))
                AddCell (m, double (i), double (j));
    return m;
}

} // namespace

// Adjacent faces of a welded cube share vertex indexes. Provenance keeps them
// distinct, and each face's area is the triangle sum regardless of sharing.
TEST (SurfaceSourceMap, WeldedVerticesDoNotMergeOrCorruptFaces)
{
    const Mesh cube = SharedVertexCube ();
    std::string reason;
    const std::vector<SourceFace> faces = EnumerateSourceFaces (cube, &reason);
    ASSERT_TRUE (reason.empty ()) << reason;
    ASSERT_EQ (faces.size (), 6u);
    for (const SourceFace& face : faces)
        EXPECT_DOUBLE_EQ (face.areaM2, 1.0) << "body " << face.sourceBody << " pgon " << face.sourcePgon;

    double area = 0.0;
    ASSERT_TRUE (AnalyticArea (cube, 1, 2, &area, &reason)) << reason;
    EXPECT_DOUBLE_EQ (area, 1.0);

    FaceQuery query;
    query.points = { 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1 };
    const auto match = Resolve (cube, query);
    ASSERT_EQ (match.status, MatchStatus::Ok) << match.reason;
    EXPECT_EQ (match.face.sourceBody, 1);
    EXPECT_EQ (match.face.sourcePgon, 2);
}

// A native convex decomposition may subdivide only one side of an internal
// edge. The long edge and its two opposite subedges still cancel exactly.
TEST (SurfaceSourceMap, TJunctionSubdivisionPreservesCompleteBoundaryAndArea)
{
    Mesh mesh;
    mesh.vertices = { 0, 0, 0, 2, 0, 0, 2, 1, 0, 0, 1, 0, 1, 1, 0, 1, 2, 0, 0, 2, 0 };
    mesh.triangles = { 0, 1, 2, 0, 2, 3, 3, 4, 5, 3, 5, 6 };
    mesh.triSourceBody.assign (4, 1);
    mesh.triSourcePgon.assign (4, 5);
    std::string reason;
    double area = 0.0;
    ASSERT_TRUE (AnalyticArea (mesh, 1, 5, &area, &reason)) << reason;
    EXPECT_DOUBLE_EQ (area, 3.0);
    FaceQuery query;
    query.points = { 0, 0, 0, 2, 0, 0, 2, 1, 0, 1, 1, 0, 1, 2, 0, 0, 2, 0 };
    const auto match = Resolve (mesh, query);
    ASSERT_EQ (match.status, MatchStatus::Ok) << match.reason;
    EXPECT_EQ (match.face.sourcePgon, 5);
    EXPECT_DOUBLE_EQ (match.face.areaM2, 3.0);
}

TEST (SurfaceSourceMap, TJunctionDoesNotHideDuplicatedTriangles)
{
    Mesh mesh;
    mesh.vertices = { 0, 0, 0, 2, 0, 0, 2, 1, 0, 0, 1, 0, 1, 1, 0, 1, 2, 0, 0, 2, 0 };
    mesh.triangles = { 0, 1, 2, 0, 2, 3, 3, 4, 5, 3, 5, 6, 3, 4, 5 };
    mesh.triSourceBody.assign (5, 1);
    mesh.triSourcePgon.assign (5, 1);
    std::string reason;
    EXPECT_TRUE (EnumerateSourceFaces (mesh, &reason).empty ());
    EXPECT_FALSE (reason.empty ());
}

TEST (SurfaceSourceMap, BoundarySubdivisionBudgetRefusesWithoutPartialMatch)
{
    FaceQuery query;
    for (int i = 0; i < 1500; ++i) {
        const double angle = i * 6.283185307179586 / 1500;
        query.points.insert (query.points.end (), { 10 * std::cos (angle), 10 * std::sin (angle), 0 });
    }
    const auto match = Resolve (Square (0, 0, 0, 1, 1), query);
    EXPECT_EQ (match.status, MatchStatus::Invalid);
    EXPECT_NE (match.reason.find ("budget"), std::string::npos);
}

TEST (SurfaceSourceMap, QueryIgnoresMalformedFaceOnlyWhenGeometricallyExcluded)
{
    Mesh mesh = Square (0, 0, 0, 1, 1);
    const uint32_t base = static_cast<uint32_t> (mesh.VertexCount ());
    mesh.vertices.insert (mesh.vertices.end (), { 10, 10, 0, 11, 10, 0, 12, 10, 0 });
    mesh.triangles.insert (mesh.triangles.end (), { base, base + 1, base + 2 });
    mesh.triSourceBody.push_back (1);
    mesh.triSourcePgon.push_back (2);
    std::string reason;
    EXPECT_TRUE (EnumerateSourceFaces (mesh, &reason).empty ()); // whole-element inventory stays strict
    EXPECT_FALSE (reason.empty ());
    const auto matched = Resolve (mesh, FaceQuery { SquarePoints (0, 0, 0), 0, 0 });
    ASSERT_EQ (matched.status, MatchStatus::Ok) << matched.reason;
    EXPECT_EQ (matched.face.sourcePgon, 1);
    EXPECT_DOUBLE_EQ (matched.face.areaM2, 1.0);
}

TEST (SurfaceSourceMap, QueryRefusesMalformedCandidateInsteadOfDroppingIt)
{
    Mesh mesh = Square (0, 0, 0, 1, 1);
    const uint32_t base = static_cast<uint32_t> (mesh.VertexCount ());
    mesh.vertices.insert (mesh.vertices.end (), { 0.2, 0.2, 0, 0.4, 0.2, 0, 0.6, 0.2, 0 });
    mesh.triangles.insert (mesh.triangles.end (), { base, base + 1, base + 2 });
    mesh.triSourceBody.push_back (1);
    mesh.triSourcePgon.push_back (2);
    const auto matched = Resolve (mesh, FaceQuery { SquarePoints (0, 0, 0), 0, 0 });
    EXPECT_EQ (matched.status, MatchStatus::Invalid);
    EXPECT_NE (matched.reason.find ("degenerate"), std::string::npos);
}

TEST (SurfaceSourceMap, QueryScopeStillValidatesGlobalProvenanceAndIndices)
{
    Mesh mesh = Square (0, 0, 0, 1, 1);
    mesh.vertices.insert (mesh.vertices.end (), { 10, 10, 0, 11, 10, 0, 12, 10, 0 });
    mesh.triangles.insert (mesh.triangles.end (), { 4, 5, 99 });
    mesh.triSourceBody.push_back (1);
    mesh.triSourcePgon.push_back (2);
    const auto matched = Resolve (mesh, FaceQuery { SquarePoints (0, 0, 0), 0, 0 });
    EXPECT_EQ (matched.status, MatchStatus::Invalid);
    EXPECT_NE (matched.reason.find ("missing vertex"), std::string::npos);
}

TEST (SurfaceSourceMap, QueryScopeUsesAllPiecesAndDoesNotLetIdsBreakTies)
{
    Mesh mesh = Square (0, 0, 0, 1, 1);
    const uint32_t base = static_cast<uint32_t> (mesh.VertexCount ());
    const Mesh duplicate = Square (0, 0, 0, 2, 1);
    mesh.vertices.insert (mesh.vertices.end (), duplicate.vertices.begin (), duplicate.vertices.end ());
    for (uint32_t index : duplicate.triangles)
        mesh.triangles.push_back (base + index);
    mesh.triSourceBody.insert (mesh.triSourceBody.end (), { 2, 2 });
    mesh.triSourcePgon.insert (mesh.triSourcePgon.end (), { 1, 1 });
    FaceQuery query;
    query.expectedBody = 2;
    query.pieces = { { 0, 0, 0, 1, 0, 0, 1, 1, 0 }, { 0, 0, 0, 1, 1, 0, 0, 1, 0 } };
    EXPECT_EQ (Resolve (mesh, query).status, MatchStatus::Ambiguous);
}

// Two bodies, each with a polygon id of 1, geometrically apart. The complete
// boundary resolves the right one; the ids only confirm the unique match.
TEST (SurfaceSourceMap, MultiBodySharedPolygonIdResolvesByGeometryAndId)
{
    Mesh mesh;
    auto a = Square (0.0, 0.0, 0.0, 1, 1);
    auto b = Square (2.0, 2.0, 0.0, 2, 1);
    mesh.vertices = a.vertices;
    mesh.vertices.insert (mesh.vertices.end (), b.vertices.begin (), b.vertices.end ());
    mesh.triangles = a.triangles;
    for (uint32_t index : b.triangles)
        mesh.triangles.push_back (index + 4);
    mesh.triMaterial.assign (4, 0);
    mesh.triSourceBody = { 1, 1, 2, 2 };
    mesh.triSourcePgon = { 1, 1, 1, 1 };

    std::string reason;
    const std::vector<SourceFace> faces = EnumerateSourceFaces (mesh, &reason);
    ASSERT_TRUE (reason.empty ()) << reason;
    ASSERT_EQ (faces.size (), 2u);
    EXPECT_DOUBLE_EQ (faces[0].areaM2, 1.0);
    EXPECT_DOUBLE_EQ (faces[1].areaM2, 1.0);

    FaceQuery query;
    query.points = SquarePoints (2.0, 2.0, 0.0);
    query.expectedBody = 2;
    query.expectedPgon = 1;
    const auto ok = Resolve (mesh, query);
    ASSERT_EQ (ok.status, MatchStatus::Ok) << ok.reason;
    EXPECT_EQ (ok.face.sourceBody, 2);

    // The same unique geometry with the WRONG expected body is a contradiction.
    FaceQuery contradiction = query;
    contradiction.expectedBody = 1;
    EXPECT_EQ (Resolve (mesh, contradiction).status, MatchStatus::Invalid);
}

// ⚠️ IDS NEVER BREAK A GEOMETRIC TIE. Two bodies with identical geometry stay
// Ambiguous even when the caller supplies the expected body: the helper will not
// use a number to choose between two faces it cannot tell apart.
TEST (SurfaceSourceMap, DuplicateGeometricFacesWithExpectedIdsRemainAmbiguous)
{
    Mesh mesh;
    auto a = Square (0.0, 0.0, 0.0, 1, 1);
    auto b = Square (0.0, 0.0, 0.0, 2, 1); // same plane, same shape, other body
    mesh.vertices = a.vertices;
    mesh.vertices.insert (mesh.vertices.end (), b.vertices.begin (), b.vertices.end ());
    mesh.triangles = a.triangles;
    for (uint32_t index : b.triangles)
        mesh.triangles.push_back (index + 4);
    mesh.triMaterial.assign (4, 0);
    mesh.triSourceBody = { 1, 1, 2, 2 };
    mesh.triSourcePgon = { 1, 1, 1, 1 };

    FaceQuery query;
    query.points = SquarePoints (0.0, 0.0, 0.0);
    query.expectedBody = 2;
    query.expectedPgon = 1;
    EXPECT_EQ (Resolve (mesh, query).status, MatchStatus::Ambiguous);
}

// ⚠️ PLANE + AREA + CENTROID + NORMAL ARE NOT ENOUGH. A unit square and a
// diamond of the same area and centroid differ only in their boundary.
TEST (SurfaceSourceMap, EqualAreaEqualCentroidDifferentContourIsNoMatch)
{
    const Mesh mesh = Square (0.0, 0.0, 0.0, 1, 1);
    const double h = std::sqrt (2.0) / 2.0; // half-diagonal for area 1, centroid (0.5, 0.5)
    FaceQuery diamond;
    diamond.points = { 0.5 - h, 0.5, 0.0, 0.5, 0.5 - h, 0.0, 0.5 + h, 0.5, 0.0, 0.5, 0.5 + h, 0.0 };
    EXPECT_EQ (Resolve (mesh, diamond).status, MatchStatus::NoMatch);
}

// A bent face is refused rather than having its vector area silently projected.
TEST (SurfaceSourceMap, NonPlanarInventoryIsInvalidAndPlanarQueryCannotMatch)
{
    Mesh mesh;
    mesh.vertices = { 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 1, 0 }; // (1,1,0) raised to z = 1
    mesh.triangles = { 0, 1, 2, 0, 2, 3 };
    mesh.triMaterial.assign (2, 0);
    mesh.triSourceBody.assign (2, 1);
    mesh.triSourcePgon.assign (2, 1);

    std::string reason;
    EXPECT_TRUE (EnumerateSourceFaces (mesh, &reason).empty ());
    EXPECT_FALSE (reason.empty ());
    EXPECT_EQ (Resolve (mesh, FaceQuery { SquarePoints (0.0, 0.0, 0.0), 0, 0 }).status, MatchStatus::NoMatch);
}

// A query contour that is not planar is refused.
TEST (SurfaceSourceMap, NonPlanarQueryIsInvalid)
{
    const Mesh mesh = Square (0.0, 0.0, 0.0, 1, 1);
    FaceQuery bent;
    bent.points = { 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 1, 0 };
    EXPECT_EQ (Resolve (mesh, bent).status, MatchStatus::Invalid);
}

// Coordinates that are finite but overflow the area arithmetic are refused, not
// turned into an infinite area.
TEST (SurfaceSourceMap, HugeFiniteCoordsAreRefused)
{
    Mesh mesh;
    mesh.vertices = { 0, 0, 0, 1e200, 0, 0, 0, 1e200, 0 };
    mesh.triangles = { 0, 1, 2 };
    mesh.triMaterial = { 0 };
    mesh.triSourceBody = { 1 };
    mesh.triSourcePgon = { 1 };

    std::string reason;
    EXPECT_TRUE (EnumerateSourceFaces (mesh, &reason).empty ());
    EXPECT_FALSE (reason.empty ());

    const Mesh square = Square (0.0, 0.0, 0.0, 1, 1);
    geomsrv::surfacemap::MatchTolerance bad;
    bad.vertexTolerance = 1.0e-300; // valid tolerance but a key overflow is refused too
    EXPECT_EQ (Resolve (square, FaceQuery { SquarePoints (0.0, 0.0, 0.0), 0, 0 }, bad).status, MatchStatus::Invalid);
}

// A malformed tolerance is refused before any geometry is trusted.
TEST (SurfaceSourceMap, MalformedToleranceIsRefused)
{
    const Mesh mesh = Square (0.0, 0.0, 0.0, 1, 1);
    const FaceQuery query { SquarePoints (0.0, 0.0, 0.0), 0, 0 };

    geomsrv::surfacemap::MatchTolerance zeroVertex;
    zeroVertex.vertexTolerance = 0.0;
    EXPECT_EQ (Resolve (mesh, query, zeroVertex).status, MatchStatus::Invalid);

    geomsrv::surfacemap::MatchTolerance nanPlane;
    nanPlane.planeDistance = std::nan ("");
    EXPECT_EQ (Resolve (mesh, query, nanPlane).status, MatchStatus::Invalid);

    geomsrv::surfacemap::MatchTolerance badNormal;
    badNormal.normalDot = 1.5;
    EXPECT_EQ (Resolve (mesh, query, badNormal).status, MatchStatus::Invalid);
}

// Source area sums the emitted triangle magnitudes; a plain fan of a solid
// polygon is the polygon's area.
TEST (SurfaceSourceMap, AnalyticAreaSumsTriangleMagnitudes)
{
    Mesh mesh;
    mesh.vertices = { 0, 0, 0, 10, 0, 0, 10, 10, 0, 0, 10, 0 };
    mesh.triangles = { 0, 1, 2, 0, 2, 3 };
    mesh.triMaterial.assign (2, 0);
    mesh.triSourceBody.assign (2, 1);
    mesh.triSourcePgon.assign (2, 1);

    double area = 0.0;
    std::string reason;
    ASSERT_TRUE (AnalyticArea (mesh, 1, 1, &area, &reason)) << reason;
    EXPECT_NEAR (area, 100.0, 1e-9);
}

// ⚠️ A HOLE IS EXPRESSED BY THE TESSELLATION, NOT BY INVERSE-WOUND TRIANGLES.
// A source mesh whose triangles mix winding is refused; a hole belongs on the
// query contour, and a convex-part decomposition of the same region matches it.
TEST (SurfaceSourceMap, ConvexDecompositionMatchesOuterAndHoleQuery)
{
    const Mesh ring = Ring ();
    double ringArea = 0.0;
    std::string reason;
    ASSERT_TRUE (AnalyticArea (ring, 1, 1, &ringArea, &reason)) << reason;
    EXPECT_NEAR (ringArea, 12.0, 1e-9); // [0,4]^2 minus [1,3]^2

    FaceQuery query;
    query.points = { 0, 0, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0 };
    query.pieces.push_back ({ 1, 1, 0, 1, 3, 0, 3, 3, 0, 3, 1, 0 }); // hole, opposite winding

    const auto match = Resolve (ring, query);
    ASSERT_EQ (match.status, MatchStatus::Ok) << match.reason;
    EXPECT_EQ (match.face.sourceBody, 1);
    EXPECT_EQ (match.face.sourcePgon, 1);

    // The same region given as the hole wound the other way does NOT match.
    FaceQuery solid = query;
    solid.points = { 0, 0, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0 };
    EXPECT_EQ (Resolve (Square (0.0, 0.0, 0.0, 1, 1), solid).status, MatchStatus::NoMatch);
}

// Inverse-wound triangles inside a SOURCE face are refused: the emitted
// tessellation is always consistently wound, so a mixed one is not trusted.
TEST (SurfaceSourceMap, InverseWoundSourceTrianglesAreRefused)
{
    Mesh mesh;
    mesh.vertices = { 0, 0, 0, 10, 0, 0, 10, 10, 0, 0, 10, 0, 4, 4, 0, 4, 6, 0, 6, 6, 0, 6, 4, 0 };
    mesh.triangles = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 }; // hole wound the SAME way
    mesh.triMaterial.assign (4, 0);
    mesh.triSourceBody.assign (4, 1);
    mesh.triSourcePgon.assign (4, 1);
    EXPECT_EQ (Resolve (mesh, FaceQuery { SquarePoints (0.0, 0.0, 0.0), 0, 0 }).status, MatchStatus::Invalid);
}

// ⚠️ A SHARED INTERIOR EDGE MUST BE TRAVERSED BOTH WAYS. Two identical
// same-wound source triangles share every edge in the SAME direction; cancelling
// them would hide the duplicate and inflate the matched area.
TEST (SurfaceSourceMap, DuplicateSameWoundSourceTriangleIsRefused)
{
    Mesh mesh;
    mesh.vertices = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    mesh.triangles = { 0, 1, 2, 0, 1, 2 }; // the same triangle twice, same winding
    mesh.triMaterial.assign (2, 0);
    mesh.triSourceBody.assign (2, 1);
    mesh.triSourcePgon.assign (2, 1);

    std::string reason;
    EXPECT_TRUE (EnumerateSourceFaces (mesh, &reason).empty ());
    EXPECT_FALSE (reason.empty ());
    EXPECT_EQ (Resolve (mesh, FaceQuery { SquarePoints (0.0, 0.0, 0.0), 0, 0 }).status, MatchStatus::Invalid);
}

// A duplicated outer piece beside a valid region must be refused too: the valid
// single-outer query matches, the duplicate does not silently cancel.
TEST (SurfaceSourceMap, DuplicateSameWoundQueryPieceIsRefused)
{
    const Mesh mesh = Square (0.0, 0.0, 0.0, 1, 1);

    FaceQuery single;
    single.points = SquarePoints (0.0, 0.0, 0.0);
    ASSERT_EQ (Resolve (mesh, single).status, MatchStatus::Ok);

    FaceQuery duplicate;
    duplicate.points = SquarePoints (0.0, 0.0, 0.0);
    duplicate.pieces.push_back (SquarePoints (0.0, 0.0, 0.0)); // duplicate outer, same winding
    EXPECT_EQ (Resolve (mesh, duplicate).status, MatchStatus::Invalid);
}

TEST (SurfaceSourceMap, HoleCrossingOuterContourIsRefused)
{
    FaceQuery query;
    query.points = { 0, 0, 0, 2, 0, 0, 2, 2, 0, 0, 2, 0 };
    query.pieces.push_back (SquarePoints (1.4, 0.5, 0, true));
    EXPECT_EQ (Resolve (Square (0, 0, 0, 1, 1), query).status, MatchStatus::Invalid);
}

TEST (SurfaceSourceMap, IntersectingHoleContoursAreRefused)
{
    FaceQuery query;
    query.points = { 0, 0, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0 };
    query.pieces = { SquarePoints (1, 1, 0, true), SquarePoints (1.5, 1.5, 0, true) };
    EXPECT_EQ (Resolve (Square (0, 0, 0, 1, 1), query).status, MatchStatus::Invalid);
}

TEST (SurfaceSourceMap, SurveyPlacedContourUsesLocalAreaArithmetic)
{
    const auto mesh = Square (2000000, 5000000, 100, 1, 1);
    FaceQuery query { SquarePoints (2000000, 5000000, 100), 0, 0 };
    const auto match = Resolve (mesh, query);
    ASSERT_EQ (match.status, MatchStatus::Ok) << match.reason;
    EXPECT_DOUBLE_EQ (match.face.areaM2, 1.0);
}

// Two disjoint outers are not one outer with a hole. Same-wound is refused by
// the winding rule; an oppositely wound disjoint outer is refused for not being
// enclosed.
TEST (SurfaceSourceMap, DisjointLargerAndSmallerOutersAreRefused)
{
    const Mesh mesh = Square (0.0, 0.0, 0.0, 1, 1);

    FaceQuery sameWound;
    sameWound.pieces.push_back ({ 0, 0, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0 });         // large CCW
    sameWound.pieces.push_back ({ 10, 10, 0, 11, 10, 0, 11, 11, 0, 10, 11, 0 }); // small CCW, disjoint
    EXPECT_EQ (Resolve (mesh, sameWound).status, MatchStatus::Invalid);

    FaceQuery oppositeWound;
    oppositeWound.pieces.push_back ({ 0, 0, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0 });         // large CCW
    oppositeWound.pieces.push_back ({ 10, 10, 0, 10, 11, 0, 11, 11, 0, 11, 10, 0 }); // small CW, disjoint
    EXPECT_EQ (Resolve (mesh, oppositeWound).status, MatchStatus::Invalid);
}

// A hole wound the same way as the outer is not a hole.
TEST (SurfaceSourceMap, SameWoundHoleIsRefused)
{
    const Mesh mesh = Square (0.0, 0.0, 0.0, 1, 1);
    FaceQuery query;
    query.points = { 0, 0, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0 };
    query.pieces.push_back ({ 1, 1, 0, 3, 1, 0, 3, 3, 0, 1, 3, 0 }); // same winding as the outer
    EXPECT_EQ (Resolve (mesh, query).status, MatchStatus::Invalid);
}

// A huge but finite coordinate can overflow the area-weighted centroid while the
// area itself stays finite; the face must be refused rather than emitted with an
// infinite centroid.
TEST (SurfaceSourceMap, NonFiniteCentroidIsRefused)
{
    Mesh mesh;
    mesh.vertices = { 0, 0, 0, 1e104, 0, 0, 0, 1e104, 0 };
    mesh.triangles = { 0, 1, 2 };
    mesh.triMaterial = { 0 };
    mesh.triSourceBody = { 1 };
    mesh.triSourcePgon = { 1 };

    std::string reason;
    EXPECT_TRUE (EnumerateSourceFaces (mesh, &reason).empty ());
    EXPECT_FALSE (reason.empty ());
    EXPECT_EQ (Resolve (mesh, FaceQuery { SquarePoints (0.0, 0.0, 0.0), 0, 0 }).status, MatchStatus::Invalid);
}

// The out-parameter reason is cleared on success: a failure must not leave a
// stale message for a later, successful call.
TEST (SurfaceSourceMap, ReasonClearsOnSuccess)
{
    std::string reason = "stale";
    Mesh bad = Square (0.0, 0.0, 0.0, 1, 1);
    bad.triSourceBody.clear ();
    bad.triSourcePgon.clear ();
    EXPECT_TRUE (EnumerateSourceFaces (bad, &reason).empty ());
    EXPECT_FALSE (reason.empty ());

    const Mesh good = Square (0.0, 0.0, 0.0, 1, 1);
    EXPECT_EQ (EnumerateSourceFaces (good, &reason).size (), 1u);
    EXPECT_TRUE (reason.empty ()) << reason;

    double area = 0.0;
    reason = "stale";
    EXPECT_TRUE (AnalyticArea (good, 1, 1, &area, &reason));
    EXPECT_TRUE (reason.empty ()) << reason;
}

// A decomposed CONVEX PART query (pieces, no outer) is a union boundary equal to
// the solid face it tiles.
TEST (SurfaceSourceMap, DecomposedConvexPartsMatchTheSolidFace)
{
    const Mesh mesh = Square (0.0, 0.0, 0.0, 1, 1);
    FaceQuery query;
    query.pieces.push_back ({ 0, 0, 0, 1, 0, 0, 1, 1, 0 });
    query.pieces.push_back ({ 0, 0, 0, 1, 1, 0, 0, 1, 0 });
    const auto match = Resolve (mesh, query);
    ASSERT_EQ (match.status, MatchStatus::Ok) << match.reason;
    EXPECT_EQ (match.face.sourcePgon, 1);
}

// A truncated or absent provenance sidecar is unusable -- not "identity".
TEST (SurfaceSourceMap, MisalignedSidecarIsInvalid)
{
    Mesh mesh = Square (0.0, 0.0, 0.0, 1, 1);
    mesh.triSourceBody.pop_back (); // 1 entry for 2 triangles

    std::string reason;
    EXPECT_TRUE (EnumerateSourceFaces (mesh, &reason).empty ());
    EXPECT_FALSE (reason.empty ());

    double area = 0.0;
    EXPECT_FALSE (AnalyticArea (mesh, 1, 1, &area, &reason));
    EXPECT_FALSE (reason.empty ());

    EXPECT_EQ (Resolve (mesh, FaceQuery { SquarePoints (0.0, 0.0, 0.0), 0, 0 }).status, MatchStatus::Invalid);
}

TEST (SurfaceSourceMap, MissingProvenanceIsInvalidNotIdentity)
{
    Mesh mesh = Square (0.0, 0.0, 0.0, 1, 1);
    mesh.triSourceBody.clear ();
    mesh.triSourcePgon.clear ();
    EXPECT_EQ (Resolve (mesh, FaceQuery { SquarePoints (0.0, 0.0, 0.0), 0, 0 }).status, MatchStatus::Invalid);
}

TEST (SurfaceSourceMap, OutOfRangeTriangleIndexIsInvalid)
{
    Mesh mesh = Square (0.0, 0.0, 0.0, 1, 1);
    mesh.triangles[0] = 99;
    EXPECT_EQ (Resolve (mesh, FaceQuery { SquarePoints (0.0, 0.0, 0.0), 0, 0 }).status, MatchStatus::Invalid);
}

TEST (SurfaceSourceMap, NonFinitePositionIsInvalid)
{
    Mesh mesh = Square (0.0, 0.0, 0.0, 1, 1);
    mesh.vertices[1] = std::nan ("");
    EXPECT_EQ (Resolve (mesh, FaceQuery { SquarePoints (0.0, 0.0, 0.0), 0, 0 }).status, MatchStatus::Invalid);
}

TEST (SurfaceSourceMap, NonPositiveProvenanceIdIsInvalid)
{
    Mesh mesh = Square (0.0, 0.0, 0.0, 1, 1);
    mesh.triSourcePgon[1] = 0;
    EXPECT_EQ (Resolve (mesh, FaceQuery { SquarePoints (0.0, 0.0, 0.0), 0, 0 }).status, MatchStatus::Invalid);
}

TEST (SurfaceSourceMap, NegativeExpectedIdIsInvalid)
{
    const Mesh mesh = Square (0.0, 0.0, 0.0, 1, 1);
    FaceQuery query;
    query.points = SquarePoints (0.0, 0.0, 0.0);
    query.expectedBody = -1;
    EXPECT_EQ (Resolve (mesh, query).status, MatchStatus::Invalid);
}

// Geometry that matches no face is NoMatch.
TEST (SurfaceSourceMap, UnmatchedGeometryIsNoMatch)
{
    const Mesh mesh = Square (0.0, 0.0, 0.0, 1, 1);
    EXPECT_EQ (Resolve (mesh, FaceQuery { SquarePoints (5.0, 5.0, 0.0), 0, 0 }).status, MatchStatus::NoMatch);
}

// Same plane, opposite winding: same-sense matching refuses (do not guess).
TEST (SurfaceSourceMap, OppositeSenseIsNoMatch)
{
    const Mesh mesh = Square (0.0, 0.0, 0.0, 1, 1);
    EXPECT_EQ (Resolve (mesh, FaceQuery { SquarePoints (0.0, 0.0, 0.0, true), 0, 0 }).status, MatchStatus::NoMatch);
}

// A degenerate query (collinear points) is unusable, never a match.
TEST (SurfaceSourceMap, DegenerateQueryIsInvalid)
{
    const Mesh mesh = Square (0.0, 0.0, 0.0, 1, 1);
    EXPECT_EQ (Resolve (mesh, FaceQuery { { 0, 0, 0, 1, 0, 0, 2, 0, 0 }, 0, 0 }).status, MatchStatus::Invalid);
}

// Ids without geometry cannot resolve anything.
TEST (SurfaceSourceMap, IdAloneDoesNotMatchWithoutGeometry)
{
    const Mesh mesh = Square (0.0, 0.0, 0.0, 1, 1);
    FaceQuery query;
    query.expectedBody = 1;
    query.expectedPgon = 1;
    EXPECT_EQ (Resolve (mesh, query).status, MatchStatus::Invalid);
}

// An empty mesh is valid and simply has no faces.
TEST (SurfaceSourceMap, EmptyMeshHasNoFaces)
{
    Mesh mesh;
    std::string reason;
    EXPECT_TRUE (EnumerateSourceFaces (mesh, &reason).empty ());
    EXPECT_TRUE (reason.empty ());
    double area = 0.0;
    EXPECT_FALSE (AnalyticArea (mesh, 1, 1, &area, &reason));
    EXPECT_FALSE (reason.empty ());
}

TEST (SurfaceSourceMap, ExistingMeshWithAbsentSourceFaceCannotSupplyAnArea)
{
    double area = -1;
    std::string reason;
    EXPECT_FALSE (AnalyticArea (Square (0, 0, 0, 1, 1), 1, 99, &area, &reason));
    EXPECT_EQ (reason, "no face with body 1 polygon 99");
    EXPECT_DOUBLE_EQ (area, -1);
}

TEST (SurfaceSourceMap, ReusedBoundaryOutputsAreClearedBeforeSuccess)
{
    namespace boundary = geomsrv::surfacemap::boundary;
    boundary::Frame frame;
    const double normal[] = { 0, 0, 1 };
    const double origin[] = { 0, 0, 0 };
    std::string reason;
    ASSERT_TRUE (boundary::MakeFrame (normal, origin, frame, reason));
    std::vector<boundary::Loop> loops;
    const std::vector<std::vector<double>> pieces { SquarePoints (0, 0, 0) };
    ASSERT_TRUE (boundary::BuildCanonicalLoops (pieces, frame, 1e-4, loops, reason));
    const auto expected = loops;
    reason = "stale failure";
    ASSERT_TRUE (boundary::BuildCanonicalLoops (pieces, frame, 1e-4, loops, reason));
    EXPECT_EQ (loops, expected);
    EXPECT_TRUE (reason.empty ());
}
