// Pure Stage B area/fragment contract. Analytic fixtures, not live acceptance.
#include "Geometry/ExposedSurface.hpp"
#include "Geometry/SurfaceCoverage.hpp"
#include "MeshFixtures.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <tuple>
#include <future>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace geomsrv;
namespace ex = geomsrv::exposed;
using V = engine::Vector3;

Mesh Box (const std::string& guid, double x, double y, double z, double sx, double sy, double sz, int32_t material = 10,
          int32_t body = 1)
{
    Mesh mesh = evptest::MakeBox (guid, x, y, z, sx, sy, sz);
    mesh.triMaterial.assign (12, material);
    mesh.triSourceBody.assign (12, body);
    for (int32_t i = 1; i <= 6; ++i)
        mesh.triSourcePgon.insert (mesh.triSourcePgon.end (), { i, i });
    return mesh;
}

void Quad (Mesh& mesh, const V& a, const V& b, const V& c, const V& d, int32_t polygon, int32_t material)
{
    const uint32_t base = static_cast<uint32_t> (mesh.VertexCount ());
    for (const auto& p : { a, b, c, d })
        mesh.vertices.insert (mesh.vertices.end (), { p.x, p.y, p.z });
    mesh.triangles.insert (mesh.triangles.end (), { base, base + 1, base + 2, base, base + 2, base + 3 });
    mesh.triSourceBody.insert (mesh.triSourceBody.end (), { 1, 1 });
    mesh.triSourcePgon.insert (mesh.triSourcePgon.end (), { polygon, polygon });
    mesh.triMaterial.insert (mesh.triMaterial.end (), { material, material });
}

Mesh Opening ()
{
    Mesh mesh;
    mesh.guid = "wall-with-opening";
    const auto front = [&] (double x0, double x1, double z0, double z1) {
        Quad (mesh, { x0, 0, z0 }, { x1, 0, z0 }, { x1, 0, z1 }, { x0, 0, z1 }, 1, 10);
    };
    front (0, 5, 0, .8);
    front (0, 5, 2.3, 3);
    front (0, 1, .8, 2.3);
    front (2.2, 5, .8, 2.3);
    Quad (mesh, { 1, 0, .8 }, { 1, .2, .8 }, { 1, .2, 2.3 }, { 1, 0, 2.3 }, 2, 20);
    Quad (mesh, { 2.2, .2, .8 }, { 2.2, 0, .8 }, { 2.2, 0, 2.3 }, { 2.2, .2, 2.3 }, 3, 20);
    Quad (mesh, { 1, 0, 2.3 }, { 1, .2, 2.3 }, { 2.2, .2, 2.3 }, { 2.2, 0, 2.3 }, 4, 20);
    Quad (mesh, { 1, .2, .8 }, { 1, 0, .8 }, { 2.2, 0, .8 }, { 2.2, .2, .8 }, 5, 20);
    evptest::RecomputeBounds (mesh);
    return mesh;
}

void Append (Mesh& into, const Mesh& source, bool reverse = false)
{
    const uint32_t offset = static_cast<uint32_t> (into.VertexCount ());
    into.vertices.insert (into.vertices.end (), source.vertices.begin (), source.vertices.end ());
    for (size_t i = 0; i < source.triangles.size (); i += 3)
        for (size_t j : { size_t (0), reverse ? size_t (2) : size_t (1), reverse ? size_t (1) : size_t (2) })
            into.triangles.push_back (source.triangles[i + j] + offset);
    into.triMaterial.insert (into.triMaterial.end (), source.triMaterial.begin (), source.triMaterial.end ());
    into.triSourceBody.insert (into.triSourceBody.end (), source.triSourceBody.begin (), source.triSourceBody.end ());
    into.triSourcePgon.insert (into.triSourcePgon.end (), source.triSourcePgon.begin (), source.triSourcePgon.end ());
    evptest::RecomputeBounds (into);
}

Snapshot Capture (std::vector<Mesh> meshes)
{
    Snapshot snapshot;
    snapshot.id = 19;
    snapshot.captureStamp = 7;
    snapshot.meshes = std::move (meshes);
    return snapshot;
}

ex::Request Request (std::vector<ex::FaceKey> faces = { { { 0, 1 }, 3 } }, std::vector<ex::BodyKey> covers = {})
{
    ex::Request request;
    request.snapshotId = 19;
    request.captureStamp = 7;
    request.coverScopeId = "explicit-test-covers/model-state-A";
    request.faces = std::move (faces);
    request.covers = std::move (covers);
    return request;
}

double EmittedArea (const ex::Result& result, bool selectedOnly = false)
{
    double total = 0;
    for (const auto& fragment : result.fragments) {
        if (selectedOnly && !fragment.selected)
            continue;
        double area = 0;
        for (size_t i = 0; i < fragment.triangles.size (); i += 3) {
            const auto point = [&] (size_t corner) {
                const size_t index = fragment.triangles[i + corner] * 3;
                return V { fragment.points[index], fragment.points[index + 1], fragment.points[index + 2] };
            };
            const V a = point (0), b = point (1), c = point (2);
            const V ab { b.x - a.x, b.y - a.y, b.z - a.z }, ac { c.x - a.x, c.y - a.y, c.z - a.z };
            const V cross = engine::Cross (ab, ac);
            area += std::sqrt (engine::Dot (cross, cross)) / 2;
        }
        EXPECT_DOUBLE_EQ (area, fragment.areaM2);
        total += area;
    }
    return total;
}

void ExpectRefusal (const Snapshot& snapshot, const ex::Request& request, const std::string& reason,
                    const ex::Limits& limits = {}, const std::function<bool ()>& cancelled = {})
{
    ex::Result result;
    result.snapshotId = 42;
    result.areas.exposedArea = 999;
    result.fragments.emplace_back ();
    std::string error;
    EXPECT_FALSE (ex::Calculate (snapshot, request, result, error, limits, cancelled));
    EXPECT_FALSE (error.empty ());
    EXPECT_NE (error.find (reason), std::string::npos) << error;
    EXPECT_EQ (result.snapshotId, 0u);
    EXPECT_TRUE (result.fragments.empty ());
    EXPECT_TRUE (result.faces.empty ());
    EXPECT_DOUBLE_EQ (result.areas.exposedArea, 0);
}
} // namespace

TEST (ExposedSurface, TriangulationPreservesBinaryGridInsteadOfResnappingToDecimalGrid)
{
    namespace cp = Clipper2Lib;
    const double grid = std::ldexp (1.0, -24); // actual ClipperD(7) lattice
    const cp::PathsD loops { { { 0.0, 0.0 }, { 10000000 * grid, 0.0 }, { 10000000 * grid, grid }, { 0.0, grid } } };
    cp::PathsD triangles;
    std::string error;
    ASSERT_TRUE (surfacecoverage::TriangulateOnGrid (loops, grid, triangles, error)) << error;
    double area = 0;
    for (const auto& triangle : triangles)
        area += std::abs (cp::Area (triangle));
    EXPECT_NEAR (area, 10000000 * grid * grid, 1e-20);
}

TEST (ExposedSurface, CapturedOneGridTriangularContactSliverIsNotDiscarded)
{
    namespace cp = Clipper2Lib;
    const double grid = std::ldexp (1.0, -24);
    const cp::Path64 captured { { 121689711, -36909875 }, { 116656546, -36909874 }, { 116656546, -36909875 },
                                { 118580124, -36909875 }, { 120850850, -36909875 }, { 121228950, -36909875 } };
    int errorCode = 0;
    const auto loops = cp::ScalePaths<double, int64_t> (cp::Paths64 { captured }, grid, errorCode);
    ASSERT_EQ (errorCode, 0);
    cp::PathsD triangles;
    std::string error;
    ASSERT_TRUE (surfacecoverage::TriangulateOnGrid (loops, grid, triangles, error)) << error;
    double area = 0;
    for (const auto& triangle : triangles)
        area += std::abs (cp::Area (triangle));
    EXPECT_NEAR (area, .5 * (121689711 - 116656546) * grid * grid, 1e-20);
    EXPECT_FALSE (triangles.empty ());
}

// #168: won-lit fragments from a slab with a cutout (captured offline from the
// reporter's scene). Nearly coincident shadow edges leave hairline spikes the
// triangulator refuses; one such surface blanked the whole won-lit surface.
// Repaired, they fill their own area, less only the hairlines.
// Refused with TriangulateResult 1 before the repair.
const std::vector<std::vector<long long>> kRefused6 = {
    { 3166643732,  -1117559008, 3329859844, -1115535272, 3494809848, -1113490036, 3670864080, -1111307116, 3670968556,
      -839171064,  3555307208,  -840382904, 3510245544,  -840849536, 3490905744,  -841049808, 3490908928,  -840866928,
      3327448216,  -842575404,  3327450240, -842554688,  3224442396, -843635560,  3161971668, -844303692,  3156805648,
      -844358944,  3127045800,  -844669988, 2825323020,  -847886044, 2825342672,  -847904088, 2948677432,  -846584968,
      2948675564,  -846578924,  2958701476, -846477756,  3156805648, -844358944,  3157686056, -844349740,  3161971668,
      -844303692,  3161971440,  -844304952, 3327448216,  -842575404, 3327432388,  -842737220, 3369619788,  -842305800,
      3490905744,  -841049808,  3486163504, -1113242168, 3309934128, -1115426924, 3144820032, -1117473880, 3119298972,
      -1117790268, 3119681304,  -1118141292 },
    { 3034662680, -1040084276, 2960109096, -971635388, 2960109096, -971635392, 2825377360, -847935940,
      2825377364, -847935940,  2825360804, -847920736, 2825360800, -847920736, 2825377360, -847935940,
      2825377360, -847935944,  2960109096, -971635392, 2998211696, -1006618024 },
    { 3108048392, -1107460920, 3110422028, -1109640196, 3112880332, -1111897208 },
};
// Refused with TriangulateResult 3 before the repair.
const std::vector<std::vector<long long>> kRefused7 = {
    { 1061221580, 428576676,  1063012352, 450663328,  1064822632, 472818292,  1071027168, 559078020,
      1083985804, 743917552,  1097327744, 933910132,  1111191180, 1130988704, 1117341768, 1221050752,
      1117341764, 1221050748, 1110175624, 1213219464, 922886220,  1008546304, 1034929072, 1130988704,
      1110175624, 1213219464, 1115439952, 1218972412, 1116120600, 1219716240, 1116495744, 1220126200,
      1117341764, 1221050748, 1111191180, 1130988704, 1110775492, 1130860052, 1097327744, 933910132,
      1096950468, 933825084,  1083985804, 743917552,  1083640456, 743868024,  1071027168, 559078020,
      1070708252, 559058024,  1064822632, 472818292,  1064524508, 472823488,  1063012352, 450663328,
      1062730396, 450690864,  1061221580, 428576676,  1060952128, 428624824,  1059433592, 406365352 },
    { 846394460, 924954760, 846317324, 924870468, 846334408, 924889136, 846334412, 924889140, 846388380, 924948120,
      846388384, 924948124 },
    { 814643280, 470315388, 814803996, 472611336, 814744008, 472622060, 814903132, 474896612, 814839956,
      474902784, 814999180, 477175304, 814932976, 477176460, 819574392, 543314816, 819521028, 543311472,
      831068928, 707648104, 831036548, 707643464, 842916516, 876561248, 842895412, 876556492, 831036548,
      707643464, 819521028, 543311472, 814932976, 477176460, 814839956, 474902784, 814744008, 472622060 },
    { 846403564, 924964712, 854589212, 933910132, 846394460, 924954760 },
};

TEST (ExposedSurface, HairlineSpikesTheTriangulatorRefusesAreRepairedNotFailed)
{
    namespace cp = Clipper2Lib;
    const double grid = std::ldexp (1.0, -24), unit = grid / 4.0;
    for (const auto* refused : { &kRefused6, &kRefused7 }) {
        cp::PathsD loops;
        for (const auto& flat : *refused) {
            cp::PathD loop;
            for (size_t i = 0; i + 1 < flat.size (); i += 2)
                loop.push_back ({ double (flat[i]) * unit, double (flat[i + 1]) * unit });
            loops.push_back (std::move (loop));
        }
        cp::PathsD triangles;
        std::string error;
        ASSERT_TRUE (surfacecoverage::TriangulateOnGrid (loops, grid, triangles, error)) << error;
        double area = 0;
        for (const auto& triangle : triangles)
            area += std::abs (cp::Area (triangle));
        const double expected = std::abs (cp::Area (cp::Union (loops, cp::FillRule::NonZero, 7)));
        EXPECT_GT (expected, 0.0);
        EXPECT_NEAR (area, expected, 1.0e-3 * expected);
    }
}

TEST (ExposedSurface, NonSeoOpposingFacesWithinOneMillimetreExcludeOnlyOverlappingPatch)
{
    for (double gap : { 0.0, .0005, .001, .001001 }) {
        const auto snapshot =
            Capture ({ Box ("target", 0, 0, 0, 2, .2, 2), Box ("unoperated-cover", .5, -1 - gap, .5, 1, 1, 1) });
        auto request = Request ({ { { 0, 1 }, 3 } }, { { 1, 1 } });
        request.contactToleranceM = .001;
        ex::Result result;
        std::string error;
        ASSERT_TRUE (ex::Calculate (snapshot, request, result, error)) << error;
        EXPECT_NEAR (result.areas.exposedArea, gap <= .001 ? 3.0 : 4.0, 1e-6) << gap;
        EXPECT_DOUBLE_EQ (result.contactToleranceM, .001);
        EXPECT_NEAR (EmittedArea (result), result.areas.exposedArea, 1e-12);
    }
}

TEST (ExposedSurface, NearContactDoesNotExpandSidewaysCoverItsOwnBodyOrReachBehindTarget)
{
    auto snapshot = Capture ({ Box ("target", 0, 0, 0, 2, .0005, 2), Box ("side-gap", 2.0005, -1.0005, 0, 1, 1, 2),
                               Box ("behind", .5, .0005, .5, 1, 1, 1) });
    auto request = Request ({ { { 0, 1 }, 3 } }, { { 0, 1 }, { 1, 1 }, { 2, 1 } });
    request.contactToleranceM = .001;
    ex::Result result;
    std::string error;
    ASSERT_TRUE (ex::Calculate (snapshot, request, result, error)) << error;
    EXPECT_NEAR (result.areas.exposedArea, 4, 1e-6);
    request.contactToleranceM = .001001;
    ExpectRefusal (snapshot, request, "Contact tolerance");
}

TEST (ExposedSurface, InclinedNearbyBoundaryClipsAtTheGapLimitRatherThanRemovingWholeFace)
{
    auto cover = Box ("sloping-near-cover", 0, -1.0005, 0, 1, 1, 1);
    for (size_t i = 0; i < cover.vertices.size (); i += 3)
        cover.vertices[i + 1] -= .001 * cover.vertices[i + 2];
    evptest::RecomputeBounds (cover);
    const auto snapshot = Capture ({ Box ("target", 0, 0, 0, 1, .2, 1), cover });
    auto request = Request ({ { { 0, 1 }, 3 } }, { { 1, 1 } });
    request.contactToleranceM = .001;
    ex::Result result;
    std::string error;
    ASSERT_TRUE (ex::Calculate (snapshot, request, result, error)) << error;
    EXPECT_NEAR (result.areas.exposedArea, .5, 1e-6);
    request.contactToleranceM = 0;
    ASSERT_TRUE (ex::Calculate (snapshot, request, result, error)) << error;
    EXPECT_NEAR (result.areas.exposedArea, 1, 1e-6);
}

TEST (ExposedSurface, AnalyticWallOpeningAndEveryRevealKeepExactAreaAndHole)
{
    const auto snapshot = Capture ({ Opening () });
    auto request = Request ();
    request.faces.clear ();
    for (int32_t polygon = 1; polygon <= 5; ++polygon)
        request.faces.push_back ({ { 0, 1 }, polygon });
    ex::Result result;
    std::string error;
    ASSERT_TRUE (ex::Calculate (snapshot, request, result, error)) << error;
    ASSERT_EQ (result.faces.size (), 5u);
    EXPECT_NEAR (result.faces[0].areas.exposedArea, 13.2, 1e-6);
    EXPECT_NEAR (result.byMaterialPool.at (10).exposedArea, 13.2, 1e-6);
    EXPECT_NEAR (result.byMaterialPool.at (20).exposedArea, 1.08, 1e-6);
    EXPECT_NEAR (result.areas.exposedArea, 14.28, 1e-6);
    EXPECT_NEAR (result.areas.grossGeometricArea, 14.28, 1e-6);
    EXPECT_DOUBLE_EQ (result.areas.exposedArea, result.areas.selectedExposedArea);
    EXPECT_NEAR (EmittedArea (result), result.areas.exposedArea, 1e-12);
    ASSERT_FALSE (result.fragments.empty ());
    EXPECT_EQ (result.fragments[0].loops.size (), 2u);
    EXPECT_NEAR (result.byElementGuid.at ("wall-with-opening").exposedArea, 14.28, 1e-6);
    // Every triangle centroid avoids the original opening on the front plane.
    for (const auto& fragment : result.fragments) {
        if (fragment.source.sourcePgon != 1)
            continue;
        for (size_t i = 0; i < fragment.points.size (); i += 9) {
            const double x = (fragment.points[i] + fragment.points[i + 3] + fragment.points[i + 6]) / 3;
            const double z = (fragment.points[i + 2] + fragment.points[i + 5] + fragment.points[i + 8]) / 3;
            EXPECT_FALSE (x > 1 + 1e-7 && x < 2.2 - 1e-7 && z > .8 + 1e-7 && z < 2.3 - 1e-7);
        }
    }
}

TEST (ExposedSurface, ContactCoveredTopBottomAndEndContributeZero)
{
    const auto snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3), Box ("above", -1, -1, 3, 7, 2, .3),
                                     Box ("below", -1, -1, -.3, 7, 2, .3), Box ("end", 5, -.1, 0, 1, .4, 3) });
    const auto request =
        Request ({ { { 0, 1 }, 1 }, { { 0, 1 }, 2 }, { { 0, 1 }, 4 } }, { { 1, 1 }, { 2, 1 }, { 3, 1 } });
    ex::Result result;
    std::string error;
    ASSERT_TRUE (ex::Calculate (snapshot, request, result, error)) << error;
    EXPECT_DOUBLE_EQ (result.areas.exposedArea, 0);
    EXPECT_NEAR (result.areas.grossGeometricArea, 2.6, 1e-9);
    EXPECT_TRUE (result.fragments.empty ());
}

TEST (ExposedSurface, PartialContactRetainsOnlyUncoveredFragment)
{
    const auto snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3), Box ("cover", 0, -.3, 0, 2, .3, 3) });
    ex::Result result;
    std::string error;
    ASSERT_TRUE (ex::Calculate (snapshot, Request ({ { { 0, 1 }, 3 } }, { { 1, 1 } }), result, error)) << error;
    EXPECT_NEAR (result.areas.grossGeometricArea, 15, 1e-9);
    EXPECT_NEAR (result.areas.exposedArea, 9, 1e-6);
    EXPECT_NEAR (EmittedArea (result), 9, 1e-6);
    for (const auto& fragment : result.fragments)
        for (size_t i = 0; i < fragment.points.size (); i += 3)
            EXPECT_GE (fragment.points[i], 2 - 1e-6);
}

TEST (ExposedSurface, ClosedCoverWithCollinearTriangleTJunctionsKeepsExactCoverage)
{
    Mesh cover = Box ("subdivided-cover", 0, -.3, 0, 2, .3, 3);
    const uint32_t a = cover.triangles[0], b = cover.triangles[1], c = cover.triangles[2];
    const uint32_t middle = static_cast<uint32_t> (cover.VertexCount ());
    for (size_t axis = 0; axis < 3; ++axis)
        cover.vertices.push_back ((cover.vertices[a * 3 + axis] + cover.vertices[b * 3 + axis]) / 2);
    cover.triangles[1] = middle;
    cover.triangles.insert (cover.triangles.end (), { middle, b, c });
    cover.triSourceBody.push_back (1);
    cover.triSourcePgon.push_back (1);
    cover.triMaterial.push_back (10);
    const auto snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3), cover });
    ex::Result result;
    std::string error;
    ASSERT_TRUE (ex::Calculate (snapshot, Request ({ { { 0, 1 }, 3 } }, { { 1, 1 } }), result, error)) << error;
    EXPECT_NEAR (result.areas.exposedArea, 9, 1e-6);
    EXPECT_NEAR (EmittedArea (result), 9, 1e-6);
    auto rotated = snapshot;
    for (auto& mesh : rotated.meshes) {
        for (size_t i = 0; i < mesh.vertices.size (); i += 3) {
            const double x = mesh.vertices[i], y = mesh.vertices[i + 1];
            mesh.vertices[i] = .6 * x - .8 * y;
            mesh.vertices[i + 1] = .8 * x + .6 * y;
        }
        evptest::RecomputeBounds (mesh);
    }
    ASSERT_TRUE (ex::Calculate (rotated, Request ({ { { 0, 1 }, 3 } }, { { 1, 1 } }), result, error)) << error;
    EXPECT_NEAR (EmittedArea (result), 9, 1e-6);
    auto cracked = snapshot;
    cracked.meshes[1].vertices[middle * 3 + 2] += 1e-5;
    ExpectRefusal (cracked, Request ({ { { 0, 1 }, 3 } }, { { 1, 1 } }), "open/non-manifold");
    // Subdivision normalization is bounded and must not conceal an actually open
    // solid or a duplicate, even when its signed volume still looks plausible.
    auto open = snapshot;
    open.meshes[1].triangles.erase (open.meshes[1].triangles.begin (), open.meshes[1].triangles.begin () + 3);
    open.meshes[1].triSourceBody.pop_back ();
    open.meshes[1].triSourcePgon.pop_back ();
    open.meshes[1].triMaterial.pop_back ();
    ExpectRefusal (open, Request ({ { { 0, 1 }, 3 } }, { { 1, 1 } }), "open/non-manifold");
    auto duplicate = snapshot;
    auto& mesh = duplicate.meshes[1];
    const std::vector<uint32_t> triangle (mesh.triangles.begin (), mesh.triangles.begin () + 3);
    mesh.triangles.insert (mesh.triangles.end (), triangle.begin (), triangle.end ());
    mesh.triSourceBody.push_back (1);
    mesh.triSourcePgon.push_back (1);
    mesh.triMaterial.push_back (10);
    ExpectRefusal (duplicate, Request ({ { { 0, 1 }, 3 } }, { { 1, 1 } }), "open/non-manifold");
}

TEST (ExposedSurface, IntersectingAndOverlappingCoversAreUnionedNotSubtractedTwice)
{
    const auto snapshot =
        Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3), Box ("a", 0, -.3, 0, 2, .5, 3), Box ("b", 1, -.3, 0, 2, .5, 3) });
    ex::Result result;
    std::string error;
    ASSERT_TRUE (ex::Calculate (snapshot, Request ({ { { 0, 1 }, 3 } }, { { 1, 1 }, { 2, 1 } }), result, error))
        << error;
    EXPECT_NEAR (result.areas.exposedArea, 6, 1e-6);
    EXPECT_NEAR (EmittedArea (result), 6, 1e-6);
}

TEST (ExposedSurface, MiddleCoverSplitsTheFaceIntoTwoMeasuredIslands)
{
    const auto snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3), Box ("middle", 2, -.3, 0, 1, .3, 3) });
    ex::Result result;
    std::string error;
    ASSERT_TRUE (ex::Calculate (snapshot, Request ({ { { 0, 1 }, 3 } }, { { 1, 1 } }), result, error)) << error;
    EXPECT_EQ (result.fragments.size (), 2u);
    EXPECT_NEAR (result.areas.exposedArea, 12, 1e-6);
    EXPECT_NEAR (EmittedArea (result), 12, 1e-6);
}

TEST (ExposedSurface, CoverageHolesRemainUncoveredNotFilled)
{
    Mesh hollow = Box ("hollow", 1, -.2, 0, 3, .4, 3);
    Append (hollow, Box ("inner", 2, -.15, 1, 1, .3, 1), true);
    const auto snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3), hollow });
    ex::Result result;
    std::string error;
    ASSERT_TRUE (ex::Calculate (snapshot, Request ({ { { 0, 1 }, 3 } }, { { 1, 1 } }), result, error)) << error;
    EXPECT_NEAR (result.areas.exposedArea, 7, 1e-6); // 15 - (9 - 1)
    EXPECT_NEAR (EmittedArea (result), 7, 1e-6);
}

TEST (ExposedSurface, DownwardHeadCoveredByRealFrameHasZeroArea)
{
    const auto snapshot = Capture ({ Opening (), Box ("frame", .99, -.01, 2.2, 1.22, .22, .1) });
    ex::Result result;
    std::string error;
    ASSERT_TRUE (ex::Calculate (snapshot, Request ({ { { 0, 1 }, 4 } }, { { 1, 1 } }), result, error)) << error;
    EXPECT_NEAR (result.areas.grossGeometricArea, .24, 1e-8);
    EXPECT_DOUBLE_EQ (result.areas.exposedArea, 0);
}

TEST (ExposedSurface, NonSeoDoorCasingSubtractsJambAndHeadOnlyWhenIncludedAsACover)
{
    for (double gap : { 0.0, .0005, .001, .001001 }) {
        // Actual wall-opening edges, not camera occlusion. The two casing bodies
        // overlap half the jamb and the complete downward head respectively.
        const auto snapshot = Capture ({ Opening (), Box ("door-casing-jamb", 1 + gap, -.01, .8, .05, .22, .75),
                                         Box ("door-casing-head", .99, -.01, 2.2, 1.22, .22, .1 - gap) });
        for (int32_t polygon : { 2, 4 }) {
            auto request = Request ({ { { 0, 1 }, polygon } });
            request.contactToleranceM = .001;
            ex::Result baseline, covered;
            std::string error;
            ASSERT_TRUE (ex::Calculate (snapshot, request, baseline, error)) << error;
            const double gross = polygon == 2 ? .3 : .24;
            EXPECT_NEAR (baseline.areas.exposedArea, gross, 1e-6);
            request.covers = { { size_t (polygon == 2 ? 1 : 2), 1 } };
            ASSERT_TRUE (ex::Calculate (snapshot, request, covered, error)) << error;
            const double exposed = gap <= .001 ? (polygon == 2 ? .15 : 0) : gross;
            EXPECT_NEAR (covered.areas.exposedArea, exposed, 1e-6) << gap;
            EXPECT_NEAR (EmittedArea (covered), covered.areas.exposedArea, 1e-12);
        }
    }
}

TEST (ExposedSurface, ManyBodyLibraryCoverRetainsSolidChecksAndChargesEachBodyScan)
{
    Mesh casing = Box ("library-door", 1, -.1, 0, 1, .1, 3);
    for (int32_t body = 2; body <= 160; ++body)
        Append (casing, Box ("hardware", 10 + body, 0, 0, .01, .01, .01, 10, body));
    auto snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3), casing });
    auto request = Request ();
    for (int32_t body = 1; body <= 160; ++body)
        request.covers.push_back ({ 1, body });
    ExpectRefusal (snapshot, request, "face/body budget"); // low-level default stays bounded at 128
    ex::Limits limits;
    limits.maxCoverBodies = 2048; // calculator's explicit library-cover budget
    ex::Result result;
    std::string error;
    ASSERT_TRUE (ex::Calculate (snapshot, request, result, error, limits)) << error;
    EXPECT_NEAR (result.areas.exposedArea, 12, 1e-6);
    EXPECT_EQ (result.covers.size (), 160u);
    EXPECT_GT (result.statistics.work, size_t (160 * 160 * 12)); // full source-sidecar scans are charged
    limits.maxWork = 100000;
    ExpectRefusal (snapshot, request, "work budget", limits);
    limits.maxWork = 2000000;
    Append (snapshot.meshes[1], Box ("bad-hardware", 200, 0, 0, .01, .01, .01, 10, 161), true);
    request.covers.push_back ({ 1, 161 });
    ExpectRefusal (snapshot, request, "positive outward-wound", limits); // even distant hardware cannot be skipped
}

TEST (ExposedSurface, GapAndUnrelatedForegroundGeometryDoNotOcclude)
{
    const auto snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3), Box ("offset", 0, -1, 0, 5, .9, 3),
                                     Box ("other-building", 0, -10, 0, 5, 3, 3) });
    ex::Result result;
    std::string error;
    ASSERT_TRUE (ex::Calculate (snapshot, Request ({ { { 0, 1 }, 3 } }, { { 1, 1 }, { 2, 1 } }), result, error))
        << error;
    EXPECT_NEAR (result.areas.exposedArea, 15, 1e-6);
    EXPECT_EQ (result.statistics.candidateCoverPairs, 0u);
}

TEST (ExposedSurface, EmptyCoverListNeverBecomesWholeModelAndOwnBodyDoesNotCoverItself)
{
    const auto snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3), Box ("touching", 0, -.3, 0, 5, .3, 3) });
    ex::Result result;
    std::string error;
    ASSERT_TRUE (ex::Calculate (snapshot, Request (), result, error)) << error;
    EXPECT_NEAR (result.areas.exposedArea, 15, 1e-6);
    ASSERT_TRUE (ex::Calculate (snapshot, Request ({ { { 0, 1 }, 3 } }, { { 0, 1 } }), result, error)) << error;
    EXPECT_NEAR (result.areas.exposedArea, 15, 1e-6);
}

TEST (ExposedSurface, OtherSourceBodyOfTheSameElementCanCoverATarget)
{
    Mesh host = Box ("composite", 0, 0, 0, 5, .2, 3);
    Append (host, Box ("part", -1, -1, 3, 7, 2, .3, 20, 2));
    const auto snapshot = Capture ({ host });
    ex::Result result;
    std::string error;
    ASSERT_TRUE (ex::Calculate (snapshot, Request ({ { { 0, 1 }, 2 } }, { { 0, 2 } }), result, error)) << error;
    EXPECT_DOUBLE_EQ (result.areas.exposedArea, 0);
}

TEST (ExposedSurface, DuplicateRequestsAndCoincidentSameMaterialFacesOwnAreaOnce)
{
    const auto snapshot = Capture ({ Box ("a", 0, 0, 0, 5, .2, 3), Box ("b", 0, 0, 0, 5, .2, 3) });
    auto request = Request ({ { { 1, 1 }, 3 }, { { 0, 1 }, 3 }, { { 1, 1 }, 3 } }, { { 0, 1 }, { 0, 1 } });
    ex::Result result;
    std::string error;
    ASSERT_TRUE (ex::Calculate (snapshot, request, result, error)) << error;
    EXPECT_EQ (result.faces.size (), 2u);
    EXPECT_EQ (result.covers.size (), 1u);
    EXPECT_NEAR (result.areas.grossGeometricArea, 30, 1e-9);
    EXPECT_NEAR (result.areas.exposedArea, 15, 1e-6);
    EXPECT_NEAR (EmittedArea (result), 15, 1e-6);
}

TEST (ExposedSurface, CoincidentDifferentMaterialsRefuseAmbiguousAssignment)
{
    const auto snapshot = Capture ({ Box ("a", 0, 0, 0, 5, .2, 3, 10), Box ("b", 0, 0, 0, 5, .2, 3, 20) });
    ExpectRefusal (snapshot, Request ({ { { 0, 1 }, 3 }, { { 1, 1 }, 3 } }), "ambiguous");
}

TEST (ExposedSurface, SurfacePoolFiltersKeepSeparateTotalsAndUseThoseSameTriangles)
{
    const auto snapshot = Capture ({ Opening () });
    auto request = Request ({ { { 0, 1 }, 1 }, { { 0, 1 }, 2 }, { { 0, 1 }, 3 }, { { 0, 1 }, 4 }, { { 0, 1 }, 5 } });
    request.materialPoolFilter = std::set<int32_t> { 20 };
    ex::Result result;
    std::string error;
    ASSERT_TRUE (ex::Calculate (snapshot, request, result, error)) << error;
    EXPECT_NEAR (result.areas.exposedArea, 14.28, 1e-6);
    EXPECT_NEAR (result.areas.selectedExposedArea, 1.08, 1e-6);
    EXPECT_DOUBLE_EQ (result.byMaterialPool.at (10).selectedExposedArea, 0);
    EXPECT_NEAR (result.byMaterialPool.at (20).selectedExposedArea, 1.08, 1e-6);
    EXPECT_NEAR (EmittedArea (result, true), result.areas.selectedExposedArea, 1e-12);
    request.materialPoolFilter = std::set<int32_t> {};
    ASSERT_TRUE (ex::Calculate (snapshot, request, result, error)) << error;
    EXPECT_DOUBLE_EQ (result.areas.selectedExposedArea, 0);
    EXPECT_NEAR (result.areas.exposedArea, 14.28, 1e-6);
}

TEST (ExposedSurface, RequestOrderIsDeterministicAndCaptureIdentityIsRetained)
{
    const auto snapshot = Capture ({ Opening () });
    auto request = Request ({ { { 0, 1 }, 2 }, { { 0, 1 }, 1 } });
    ex::Result a, b;
    std::string error;
    ASSERT_TRUE (ex::Calculate (snapshot, request, a, error)) << error;
    std::reverse (request.faces.begin (), request.faces.end ());
    ASSERT_TRUE (ex::Calculate (snapshot, request, b, error)) << error;
    EXPECT_EQ (a.snapshotId, snapshot.id);
    EXPECT_EQ (a.captureStamp, snapshot.captureStamp);
    EXPECT_EQ (a.coverScopeId, request.coverScopeId);
    ASSERT_EQ (a.fragments.size (), b.fragments.size ());
    for (size_t i = 0; i < a.fragments.size (); ++i) {
        EXPECT_EQ (a.fragments[i].source, b.fragments[i].source);
        EXPECT_EQ (a.fragments[i].points, b.fragments[i].points);
        EXPECT_EQ (a.fragments[i].triangles, b.fragments[i].triangles);
    }
}

TEST (ExposedSurface, SurveyCoordinatesDoNotInflateOrProjectArea)
{
    auto snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3), Box ("cover", 0, -.3, 0, 2, .3, 3) });
    for (auto& mesh : snapshot.meshes) {
        for (size_t i = 0; i < mesh.vertices.size (); i += 3) {
            mesh.vertices[i] += 6000000;
            mesh.vertices[i + 1] -= 3000000;
            mesh.vertices[i + 2] += 1500;
        }
        mesh.bounds = {}; // broadphase must use captured geometry, not stale bounds
    }
    ex::Result result;
    std::string error;
    ASSERT_TRUE (ex::Calculate (snapshot, Request ({ { { 0, 1 }, 3 } }, { { 1, 1 } }), result, error)) << error;
    EXPECT_NEAR (result.areas.exposedArea, 9, 1e-6);
    EXPECT_NEAR (EmittedArea (result), 9, 1e-6);
}

TEST (ExposedSurface, RotatedInclinedFacesKeepTrueThreeDimensionalArea)
{
    auto snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3), Box ("cover", 0, -.3, 0, 2, .3, 3) });
    for (auto& mesh : snapshot.meshes) {
        for (size_t i = 0; i < mesh.vertices.size (); i += 3) {
            const double x = mesh.vertices[i], y = mesh.vertices[i + 1], z = mesh.vertices[i + 2];
            const double ry = y * std::cos (.45) - z * std::sin (.45);
            mesh.vertices[i] = x * std::cos (.63) - ry * std::sin (.63);
            mesh.vertices[i + 1] = x * std::sin (.63) + ry * std::cos (.63);
            mesh.vertices[i + 2] = y * std::sin (.45) + z * std::cos (.45);
        }
    }
    ex::Result result;
    std::string error;
    ASSERT_TRUE (ex::Calculate (snapshot, Request ({ { { 0, 1 }, 3 } }, { { 1, 1 } }), result, error)) << error;
    EXPECT_NEAR (result.areas.grossGeometricArea, 15, 1e-6);
    EXPECT_NEAR (result.areas.exposedArea, 9, 1e-6);
    EXPECT_NEAR (EmittedArea (result), 9, 1e-6);
}

TEST (ExposedSurface, InvalidMaterialFilterAndDuplicateElementIdentityAreExplicitRefusals)
{
    auto snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3) });
    auto request = Request ();
    request.materialPoolFilter = std::set<int32_t> { -1 };
    ExpectRefusal (snapshot, request, "negative");
    snapshot.meshes.push_back (Box ("WALL", 10, 0, 0, 5, .2, 3));
    ExpectRefusal (snapshot, Request ({ { { 0, 1 }, 3 }, { { 1, 1 }, 3 } }), "duplicate element");
}

TEST (ExposedSurface, InwardWoundCoverCannotClaimSolidCoverage)
{
    auto snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3), Box ("cover", 0, -.3, 0, 2, .3, 3) });
    auto& indices = snapshot.meshes[1].triangles;
    for (size_t i = 0; i < indices.size (); i += 3)
        std::swap (indices[i + 1], indices[i + 2]);
    ExpectRefusal (snapshot, Request ({ { { 0, 1 }, 3 } }, { { 1, 1 } }), "outward-wound");
}

TEST (ExposedSurface, InvalidIdentityAndScopeCannotPublish)
{
    const auto snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3) });
    auto request = Request ();
    request.snapshotId = 18;
    ExpectRefusal (snapshot, request, "identity");
    request = Request ();
    request.captureStamp = 0;
    ExpectRefusal (snapshot, request, "identity");
    request = Request ();
    request.coverScopeId.clear ();
    ExpectRefusal (snapshot, request, "scope identity");
}

TEST (ExposedSurface, AbsentFacesBodiesAndSidecarsAreRefusedNotZeroRows)
{
    auto snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3) });
    ExpectRefusal (snapshot, Request ({ { { 99, 1 }, 3 } }), "mesh is absent");
    ExpectRefusal (snapshot, Request ({ { { 0, 1 }, 99 } }), "face is absent");
    ExpectRefusal (snapshot, Request ({ { { 0, 1 }, 3 } }, { { 0, 2 } }), "body is absent");
    ExpectRefusal (snapshot, Request ({ { { 0, 0 }, 3 } }), "1-based");
    snapshot.meshes[0].triSourcePgon.pop_back ();
    ExpectRefusal (snapshot, Request (), "provenance");
}

TEST (ExposedSurface, InvalidNumbersIndicesAndMaterialAssignmentsRefuseAtomically)
{
    auto snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3) });
    snapshot.meshes[0].vertices[0] = std::numeric_limits<double>::quiet_NaN ();
    ExpectRefusal (snapshot, Request (), "coordinate");
    snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3) });
    snapshot.meshes[0].triangles[0] = 999;
    ExpectRefusal (snapshot, Request (), "missing vertex");
    snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3) });
    snapshot.meshes[0].triMaterial.pop_back ();
    ExpectRefusal (snapshot, Request (), "material pool");
    snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3) });
    snapshot.meshes[0].triMaterial[5] = 20;
    ExpectRefusal (snapshot, Request (), "conflicting");
}

TEST (ExposedSurface, NonPlanarTargetAndOpenCoverRefuseInsteadOfEstimating)
{
    auto snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3), Box ("cover", 0, -.3, 0, 2, .3, 3) });
    snapshot.meshes[0].vertices[1] = -.01;
    ExpectRefusal (snapshot, Request (), "face");
    snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3), Box ("cover", 0, -.3, 0, 2, .3, 3) });
    snapshot.meshes[1].triangles.resize (33);
    snapshot.meshes[1].triSourceBody.resize (11);
    snapshot.meshes[1].triSourcePgon.resize (11);
    ExpectRefusal (snapshot, Request ({ { { 0, 1 }, 3 } }, { { 1, 1 } }), "open/non-manifold");
}

TEST (ExposedSurface, UnrelatedMalformedSourcePolygonDoesNotPoisonExplicitValidFace)
{
    Mesh mesh = Box ("wall", 0, 0, 0, 5, .2, 3);
    mesh.triangles.insert (mesh.triangles.end (), { 0, 0, 0 });
    mesh.triSourceBody.push_back (1);
    mesh.triSourcePgon.push_back (99);
    mesh.triMaterial.push_back (10);
    ex::Result result;
    std::string error;
    ASSERT_TRUE (ex::Calculate (Capture ({ mesh }), Request (), result, error)) << error;
    EXPECT_NEAR (result.areas.exposedArea, 15, 1e-6);
}

TEST (ExposedSurface, CancellationAndAllBudgetsDiscardPartialResults)
{
    const auto snapshot = Capture ({ Box ("wall", 0, 0, 0, 5, .2, 3) });
    ex::Limits limits;
    limits.maxWork = 1;
    ExpectRefusal (snapshot, Request (), "work budget", limits);
    limits = {};
    limits.maxInputTriangles = 1;
    ExpectRefusal (snapshot, Request (), "input triangle budget", limits);
    limits = {};
    limits.maxFaces = 0;
    ExpectRefusal (snapshot, Request (), "face/body budget", limits);
    limits = {};
    limits.maxOutputTriangles = 1;
    ExpectRefusal (snapshot, Request (), "output budget", limits);
    limits = {};
    limits.maxSeconds = std::numeric_limits<double>::quiet_NaN ();
    ExpectRefusal (snapshot, Request (), "time budget", limits);
    ExpectRefusal (snapshot, Request (), "cancelled", {}, [] { return true; });
    size_t polls = 0;
    ExpectRefusal (snapshot, Request (), "cancelled", {}, [&] { return ++polls > 18; });
}

// Local replay of the Stage A C/Modeler body capture, never committed project
// geometry. Format: vertex/triangle counts, xyz, triangle indices, pgon sidecar,
// native contour point count and xyz. Its source face 48 is resolved by the
// earlier complete-boundary check, not inferred from picker polygon 47.
TEST (ExposedSurface, CapturedPlanarSourceFaceMatchesNativeContourArea)
{
    const char* path = std::getenv ("TAPIOCA_EXPOSED_REPLAY");
    if (path == nullptr || *path == '\0')
        GTEST_SKIP () << "Set TAPIOCA_EXPOSED_REPLAY to the local picked-body.txt Stage A replay.";
    std::ifstream input (path);
    ASSERT_TRUE (input.good ());
    size_t vertexCount = 0, triangleCount = 0;
    input >> vertexCount >> triangleCount;
    ASSERT_GT (vertexCount, 0u);
    ASSERT_LE (vertexCount, 200000u);
    ASSERT_GT (triangleCount, 0u);
    ASSERT_LE (triangleCount, 100000u);
    Mesh mesh;
    mesh.guid = "local-captured-wall";
    mesh.vertices.resize (vertexCount * 3);
    mesh.triangles.resize (triangleCount * 3);
    mesh.triSourceBody.assign (triangleCount, 1);
    mesh.triSourcePgon.resize (triangleCount);
    // The old replay wire omits material provenance: zero is an UNKNOWN pool
    // sentinel here, not evidence for any project surface-attribute mapping.
    mesh.triMaterial.assign (triangleCount, 0);
    for (auto& value : mesh.vertices)
        input >> value;
    for (auto& value : mesh.triangles)
        input >> value;
    for (auto& value : mesh.triSourcePgon)
        input >> value;
    size_t pointCount = 0;
    input >> pointCount;
    ASSERT_GE (pointCount, 3u);
    ASSERT_LE (pointCount, 200000u);
    std::vector<V> native (pointCount);
    for (auto& p : native)
        input >> p.x >> p.y >> p.z;
    ASSERT_FALSE (input.fail ());
    V vectorArea {};
    const V origin = native.front ();
    for (size_t i = 0; i < native.size (); ++i) {
        const V a = native[i], b = native[(i + 1) % native.size ()];
        const V da { a.x - origin.x, a.y - origin.y, a.z - origin.z },
            db { b.x - origin.x, b.y - origin.y, b.z - origin.z };
        const V cross = engine::Cross (da, db);
        vectorArea.x += cross.x;
        vectorArea.y += cross.y;
        vectorArea.z += cross.z;
    }
    const double nativeArea = std::sqrt (engine::Dot (vectorArea, vectorArea)) / 2;
    EXPECT_NEAR (nativeArea, 4.6794, 1e-6);
    ex::Result result;
    std::string error;
    ASSERT_TRUE (ex::Calculate (Capture ({ mesh }), Request ({ { { 0, 1 }, 48 } }), result, error)) << error;
    EXPECT_NEAR (result.areas.grossGeometricArea, nativeArea, 1e-6);
    EXPECT_NEAR (result.areas.exposedArea, nativeArea, 1e-6);
    EXPECT_NEAR (EmittedArea (result), nativeArea, 1e-6);
}

TEST (ExposedSurface, UnrepresentableSourceDoesNotSilentlyBecomeZeroExposedArea)
{
    Mesh mesh;
    mesh.guid = "below-contact-grid";
    Quad (mesh, { 0, 0, 0 }, { 4e-8, 0, 0 }, { 4e-8, 0, 3 }, { 0, 0, 3 }, 1, 10);
    ExpectRefusal (Capture ({ mesh }), Request ({ { { 0, 1 }, 1 } }), "");
}

// #168: a won-lit fragment from the reporter's scene, on the solar cut's
// lattice (2^-26 m units). Clipper2 2.0.1's Triangulate never returned on it,
// with or without Delaunay, and the solar cut hung for good. It must cover
// its own area, and in bounded time: the watchdog turns a hang into a failure.
TEST (ExposedSurface, AFragmentClipperNeverFinishedTriangulatesInBoundedTime)
{
    namespace cp = Clipper2Lib;
    const double grid = std::ldexp (1.0, -24), unit = grid / 4.0;
    const long long xy[] = { 3242200480, -1116266632, 3378334464, -1114578952, 3486163504, -1113242168,
                             3496681892, -509514352,  2801317352, -516731288,  2663932580, -518207696,
                             2661642456, -541624080,  2647644140, -684756112,  2796016612, -820979352,
                             3115801252, -1114578952, 3119298964, -1117790260 };
    cp::PathsD loops (1);
    for (size_t i = 0; i + 1 < std::size (xy); i += 2)
        loops[0].push_back ({ double (xy[i]) * unit, double (xy[i + 1]) * unit });
    auto job = std::async (std::launch::async, [loops, grid] {
        cp::PathsD triangles;
        std::string error;
        const bool ok = surfacecoverage::TriangulateOnGrid (loops, grid, triangles, error);
        double area = 0;
        for (const auto& triangle : triangles)
            area += std::abs (cp::Area (triangle));
        return std::make_tuple (ok, area, error);
    });
    if (job.wait_for (std::chrono::seconds (20)) != std::future_status::ready) {
        ADD_FAILURE () << "TriangulateOnGrid did not return";
        std::_Exit (1); // the hung worker cannot be joined
    }
    const auto [ok, area, error] = job.get ();
    ASSERT_TRUE (ok) << error;
    const double expected = std::abs (cp::Area (loops[0]));
    EXPECT_NEAR (area, expected, 1.0e-9 * expected);
}
