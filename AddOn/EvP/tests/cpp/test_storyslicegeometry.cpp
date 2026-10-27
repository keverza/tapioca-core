// Offline checks for ArchViz/StorySliceGeometry — a storey's union outline
// turned into a ribbon and a fill.
//
// The assertions are PROPERTIES, not restatements of the arithmetic: the fill is
// checked by the AREA it covers, the chaining by whether a ring closes, and the
// dash carrier by whether arc length runs continuously along a contour. A test
// that recomputed the same expression would pass for a mirrored ribbon and an
// inside-out fill, which are exactly the two failures that produce a plausible
// picture instead of an error.

#include "ArchViz/StorySliceGeometry.hpp"
#include "ArchViz/StorySliceUnion.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <algorithm>
#include <limits>
#include <map>
#include <string>
#include <vector>

using geomsrv::Polyline;
using geomsrv::archviz::BuildIndexedSliceFill;
using geomsrv::archviz::BuildSliceFill;
using geomsrv::archviz::BuildSliceRibbon;
using geomsrv::archviz::ChainUnionSegments;
using geomsrv::archviz::SliceChain;
using geomsrv::archviz::StorySliceFillMesh;
using geomsrv::archviz::StorySliceFillVertex;
using geomsrv::archviz::StorySliceVertex;
using geomsrv::archviz::UnionArea;
using geomsrv::archviz::UnionLoops;
using geomsrv::archviz::UnionSegment;

namespace {

Polyline Rect (double x0, double y0, double x1, double y1, double z = 0.0)
{
    Polyline p;
    const double xs[5] = { x0, x1, x1, x0, x0 };
    const double ys[5] = { y0, y0, y1, y1, y0 };
    for (int i = 0; i < 5; ++i) {
        p.pts.push_back (xs[i]);
        p.pts.push_back (ys[i]);
        p.pts.push_back (z);
    }
    p.closed = true;
    return p;
}

std::vector<SliceChain> ChainsOf (const std::vector<Polyline>& loops)
{
    return ChainUnionSegments (UnionLoops (loops));
}

double FillArea (const std::vector<SliceChain>& chains)
{
    std::vector<StorySliceFillVertex> tris;
    BuildSliceFill (chains, 0.0f, tris);
    double area = 0.0;
    for (size_t i = 0; i + 2 < tris.size (); i += 3) {
        const double ax = tris[i].x, ay = tris[i].y;
        const double bx = tris[i + 1].x, by = tris[i + 1].y;
        const double cx = tris[i + 2].x, cy = tris[i + 2].y;
        area += std::fabs ((bx - ax) * (cy - ay) - (cx - ax) * (by - ay)) * 0.5;
    }
    return area;
}

} // namespace

// ---- chaining --------------------------------------------------------------

TEST (StorySliceGeometry, ASingleRectangleChainsIntoOneClosedRing)
{
    const std::vector<SliceChain> chains = ChainsOf ({ Rect (0, 0, 2, 1) });
    ASSERT_EQ (chains.size (), 1u);
    EXPECT_TRUE (chains[0].closed);
    EXPECT_EQ (chains[0].Count (), 4u); // no closing repeat is stored
}

TEST (StorySliceGeometry, DisjointRegionsChainSeparately)
{
    const std::vector<SliceChain> chains = ChainsOf ({ Rect (0, 0, 1, 1), Rect (5, 5, 6, 6) });
    ASSERT_EQ (chains.size (), 2u);
    EXPECT_TRUE (chains[0].closed);
    EXPECT_TRUE (chains[1].closed);
}

TEST (StorySliceGeometry, AnOpenCrossSectionStaysOpen)
{
    // A chain that cannot close must be RETURNED open, not dropped: a silently
    // dropped contour is a hole in the outline that nobody notices.
    std::vector<UnionSegment> segs = { { 0, 0, 1, 0 }, { 1, 0, 1, 1 } };
    const std::vector<SliceChain> chains = ChainUnionSegments (segs);
    ASSERT_EQ (chains.size (), 1u);
    EXPECT_FALSE (chains[0].closed);
    EXPECT_EQ (chains[0].Count (), 3u);
}

TEST (StorySliceGeometry, EmptyInputIsEmptyNotACrash)
{
    EXPECT_TRUE (ChainUnionSegments ({}).empty ());
}

// ---- the ribbon ------------------------------------------------------------

TEST (StorySliceGeometry, RibbonEmitsSixVerticesPerSegment)
{
    const std::vector<SliceChain> chains = ChainsOf ({ Rect (0, 0, 2, 1) });
    std::vector<StorySliceVertex> ribbon;
    const size_t added = BuildSliceRibbon (chains, 3.25f, ribbon);
    EXPECT_EQ (added, 4u * 6u); // a closed quad: 4 segments
    EXPECT_EQ (ribbon.size (), added);
    for (const StorySliceVertex& v : ribbon)
        EXPECT_FLOAT_EQ (v.z, 3.25f); // the storey's plane, reattached
}

TEST (StorySliceGeometry, RibbonPushDirectionsAreUnitAndPerpendicular)
{
    // ⚠️ THE SHADER DIVIDES BY THE PROJECTED LENGTH OF THESE, so a non-unit
    // direction is not a wrong width — it is a width that varies along the
    // contour, which reads as a tessellation artefact.
    const std::vector<SliceChain> chains = ChainsOf ({ Rect (0, 0, 2, 1) });
    std::vector<StorySliceVertex> ribbon;
    BuildSliceRibbon (chains, 0.0f, ribbon);
    ASSERT_FALSE (ribbon.empty ());
    for (const StorySliceVertex& v : ribbon) {
        EXPECT_NEAR (std::hypot (v.nx, v.ny), 1.0f, 1e-5f);
        EXPECT_NEAR (std::hypot (v.tx, v.ty), 1.0f, 1e-5f);
        EXPECT_NEAR (v.nx * v.tx + v.ny * v.ty, 0.0f, 1e-5f);
    }
}

TEST (StorySliceGeometry, ArcLengthRunsContinuouslyAroundAContour)
{
    // The dash pattern is a function of this. If it restarted per segment the
    // pattern would stutter at every junction, which reads as z-fighting rather
    // than as a line style.
    const std::vector<SliceChain> chains = ChainsOf ({ Rect (0, 0, 2, 1) });
    std::vector<StorySliceVertex> ribbon;
    BuildSliceRibbon (chains, 0.0f, ribbon);
    ASSERT_FALSE (ribbon.empty ());

    float maxArc = 0.0f;
    for (const StorySliceVertex& v : ribbon)
        maxArc = std::max (maxArc, v.arc);
    EXPECT_NEAR (maxArc, 6.0f, 1e-4f); // the full perimeter 2*(2+1)
}

TEST (StorySliceGeometry, RibbonCarriesBothSidesOfTheCentreline)
{
    // ⚠️ THE ANTIALIASING RIDES ON THIS. `side` must be exactly -1 or +1 and both
    // must appear in every quad: the pixel shader reads the INTERPOLATED value as
    // "how far across the line am I", so a quad with the same sign at all four
    // corners never crosses zero and the whole line fades to its edge colour.
    const std::vector<SliceChain> chains = ChainsOf ({ Rect (0, 0, 2, 1) });
    std::vector<StorySliceVertex> ribbon;
    BuildSliceRibbon (chains, 0.0f, ribbon);
    ASSERT_FALSE (ribbon.empty ());

    for (const StorySliceVertex& v : ribbon)
        EXPECT_TRUE (v.side == 1.0f || v.side == -1.0f);

    // Per quad (6 vertices, two triangles over 4 corners) both signs are present.
    for (size_t i = 0; i + 5 < ribbon.size (); i += 6) {
        bool positive = false, negative = false;
        for (size_t k = i; k < i + 6; ++k) {
            positive = positive || ribbon[k].side > 0.0f;
            negative = negative || ribbon[k].side < 0.0f;
        }
        EXPECT_TRUE (positive) << "quad at " << i << " has no +1 side";
        EXPECT_TRUE (negative) << "quad at " << i << " has no -1 side";
    }
}

TEST (StorySliceGeometry, SideAgreesWithTheAcrossDirection)
{
    // The sign must track the push direction, not be assigned arbitrarily: the
    // shader assumes |side| == 1 at the pushed edge, so a corner pushed to -n
    // carrying side +1 measures its distance from the centre with the wrong sign
    // and the feather lands inside the line instead of outside it.
    const std::vector<SliceChain> chains = ChainsOf ({ Rect (0, 0, 2, 1) });
    std::vector<StorySliceVertex> ribbon;
    BuildSliceRibbon (chains, 0.0f, ribbon);
    ASSERT_GE (ribbon.size (), 6u);

    // Within one quad the two distinct corner positions share an across-direction
    // up to sign; the +1 corners must all carry the SAME normal as each other.
    const StorySliceVertex& plus = ribbon[0];
    for (const StorySliceVertex& v : ribbon) {
        if (v.side > 0.0f && std::fabs (v.arc - plus.arc) < 1e-6f)
            EXPECT_NEAR (v.nx * plus.nx + v.ny * plus.ny, 1.0f, 1e-5f);
    }
}

TEST (StorySliceGeometry, ADegenerateChainAppendsNothing)
{
    SliceChain single;
    single.xy = { 1.0, 2.0 };
    std::vector<StorySliceVertex> ribbon;
    EXPECT_EQ (BuildSliceRibbon ({ single }, 0.0f, ribbon), 0u);
    EXPECT_TRUE (ribbon.empty ());
}

TEST (StorySliceGeometry, RibbonAppendsRatherThanClears)
{
    const std::vector<SliceChain> chains = ChainsOf ({ Rect (0, 0, 1, 1) });
    std::vector<StorySliceVertex> ribbon;
    BuildSliceRibbon (chains, 0.0f, ribbon);
    const size_t first = ribbon.size ();
    BuildSliceRibbon (chains, 3.0f, ribbon); // a second storey into one buffer
    EXPECT_EQ (ribbon.size (), first * 2);
}

// ---- the fill --------------------------------------------------------------

TEST (StorySliceGeometry, FillCoversARectanglesArea)
{
    EXPECT_NEAR (FillArea (ChainsOf ({ Rect (0, 0, 2, 3) })), 6.0, 1e-6);
}

TEST (StorySliceGeometry, FillOfTwoOverlappingRectanglesIsNotDoubleCounted)
{
    // 3x2 and 3x2 overlapping in a 1x1 corner -> 6 + 6 - 1.
    const double area = FillArea (ChainsOf ({ Rect (0, 0, 3, 2), Rect (2, 1, 5, 3) }));
    EXPECT_NEAR (area, 11.0, 1e-4);
}

TEST (StorySliceGeometry, FillOfAbuttingRectanglesIsTheirSum)
{
    EXPECT_NEAR (FillArea (ChainsOf ({ Rect (0, 0, 2, 2), Rect (2, 0, 4, 2) })), 8.0, 1e-4);
}

TEST (StorySliceGeometry, FillSkipsAHoleUnderEvenOdd)
{
    // ⚠️ THE CASE THE STENCIL TRICK AND THE EAR-CLIP WERE BOTH REJECTED FOR. A
    // wall with an opening cuts an inner ring at the cut plane, and the fill must
    // leave it empty. The rings are handed over with NO orientation guarantee —
    // the union classifies edges, it never orients them — so this can only work
    // by even-odd over the arrangement, which is what the decomposition does.
    SliceChain outer;
    outer.xy = { 0, 0, 10, 0, 10, 10, 0, 10 };
    outer.closed = true;
    SliceChain hole;
    hole.xy = { 3, 3, 6, 3, 6, 6, 3, 6 };
    hole.closed = true;

    EXPECT_NEAR (FillArea ({ outer, hole }), 100.0 - 9.0, 1e-4);
}

TEST (StorySliceGeometry, FillIgnoresOpenChains)
{
    SliceChain open;
    open.xy = { 0, 0, 1, 0, 1, 1 };
    open.closed = false;
    std::vector<StorySliceFillVertex> tris;
    EXPECT_EQ (BuildSliceFill ({ open }, 0.0f, tris), 0u);
}

TEST (StorySliceGeometry, FillHandlesASlantedRegion)
{
    // A triangle, whose trapezoid bands are genuinely slanted rather than
    // axis-aligned: taking the band's X from a single scanline instead of from
    // both its edges would show up here and nowhere else.
    SliceChain tri;
    tri.xy = { 0, 0, 4, 0, 0, 4 };
    tri.closed = true;
    EXPECT_NEAR (FillArea ({ tri }), 8.0, 1e-4);
}

TEST (StorySliceGeometry, FillCarriesTheStoreyPlane)
{
    std::vector<StorySliceFillVertex> tris;
    BuildSliceFill (ChainsOf ({ Rect (0, 0, 1, 1) }), 7.5f, tris);
    ASSERT_FALSE (tris.empty ());
    for (const StorySliceFillVertex& v : tris)
        EXPECT_FLOAT_EQ (v.z, 7.5f);
}

TEST (StorySliceGeometry, UnionAreaAgreesWithTheFillItDescribes)
{
    const std::vector<SliceChain> chains = ChainsOf ({ Rect (0, 0, 3, 2), Rect (2, 1, 5, 3) });
    EXPECT_NEAR (UnionArea (chains), FillArea (chains), 1e-9);
    EXPECT_NEAR (UnionArea (chains), 11.0, 1e-4);
}

TEST (StorySliceGeometry, EmptyChainsFillNothing)
{
    std::vector<StorySliceFillVertex> tris;
    EXPECT_EQ (BuildSliceFill ({}, 0.0f, tris), 0u);
    EXPECT_DOUBLE_EQ (UnionArea ({}), 0.0);
}

namespace {
SliceChain Ring (std::initializer_list<double> xy)
{
    return { xy, true };
}

double IndexedArea (const StorySliceFillMesh& mesh)
{
    double area = 0;
    for (size_t i = 0; i < mesh.indices.size (); i += 3) {
        const size_t a = 3 * mesh.indices[i], b = 3 * mesh.indices[i + 1], c = 3 * mesh.indices[i + 2];
        const double cross = (mesh.points[b] - mesh.points[a]) * (mesh.points[c + 1] - mesh.points[a + 1]) -
                             (mesh.points[c] - mesh.points[a]) * (mesh.points[b + 1] - mesh.points[a + 1]);
        EXPECT_GT (cross, 0);
        area += cross / 2;
    }
    return area;
}

bool EvenOddAt (const std::vector<SliceChain>& chains, double x, double y)
{
    bool inside = false;
    for (const auto& chain : chains) {
        if (!chain.closed)
            continue;
        for (size_t i = 0, j = chain.Count () - 1; i < chain.Count (); j = i++) {
            const double ax = chain.xy[2 * i], ay = chain.xy[2 * i + 1], bx = chain.xy[2 * j], by = chain.xy[2 * j + 1];
            if ((ay > y) != (by > y) && x < ax + (y - ay) * (bx - ax) / (by - ay))
                inside = !inside;
        }
    }
    return inside;
}
} // namespace

TEST (IndexedSliceFill, CorpusPreservesEvenOddAreaContainmentAndDeterminism)
{
    const auto outer = Ring ({ 0, 0, 10, 0, 10, 10, 0, 10 });
    const auto hole = Ring ({ 2, 2, 8, 2, 8, 8, 2, 8 });
    struct Fixture {
        const char* name;
        std::vector<SliceChain> chains;
        double area;
    };
    const std::vector<Fixture> fixtures {
        { "L", { Ring ({ 0, 0, 4, 0, 4, 1, 1, 1, 1, 4, 0, 4 }) }, 7 },
        { "U", { Ring ({ 0, 0, 4, 0, 4, 4, 3, 4, 3, 1, 1, 1, 1, 4, 0, 4 }) }, 10 },
        { "notched", { Ring ({ 0, 0, 6, 0, 6, 5, 5, 4, 5, 2, 3, 2, 3, 4, 1, 5, 0, 4 }) }, 22 },
        { "nested", { outer, hole, Ring ({ 3, 3, 7, 3, 7, 7, 3, 7 }), Ring ({ 4, 4, 6, 4, 6, 6, 4, 6 }) }, 76 },
        { "duplicate", { outer, outer }, 0 },
        { "touching", { Ring ({ 0, 0, 2, 0, 2, 2, 0, 2, 0, 0, -2, 0, -2, -2, 0, -2 }) }, 8 },
        { "disjoint", { outer, Ring ({ 20, 1, 22, 1, 22, 3, 20, 3 }), hole }, 68 },
        { "overlapXor", { outer, Ring ({ 5, 0, 15, 0, 15, 10, 5, 10 }) }, 100 },
        { "doubleWalk", { Ring ({ 0, 0, 2, 0, 2, 2, 0, 2, 0, 0, 2, 0, 2, 2, 0, 2 }) }, 0 }
    };
    for (const auto& fixture : fixtures) {
        SCOPED_TRACE (fixture.name);
        StorySliceFillMesh mesh, again;
        std::string error = "stale";
        ASSERT_TRUE (BuildIndexedSliceFill (fixture.chains, 7.5, mesh, error)) << error;
        EXPECT_TRUE (error.empty ());
        ASSERT_EQ (mesh.indices.size () % 3, 0u);
        for (auto index : mesh.indices)
            ASSERT_LT (index, mesh.points.size () / 3);
        EXPECT_NEAR (IndexedArea (mesh), fixture.area, 1e-6);
        EXPECT_NEAR (IndexedArea (mesh), FillArea (fixture.chains), 1e-4);
        ASSERT_TRUE (BuildIndexedSliceFill (fixture.chains, 7.5, again, error)) << error;
        EXPECT_EQ (mesh.points, again.points);
        EXPECT_EQ (mesh.indices, again.indices);
        for (size_t i = 2; i < mesh.points.size (); i += 3)
            EXPECT_DOUBLE_EQ (mesh.points[i], 7.5);
        for (size_t i = 0; i < mesh.indices.size (); i += 3) {
            for (size_t corner = 0; corner < 3; ++corner) {
                double x = 0, y = 0;
                for (size_t k = 0; k < 3; ++k) {
                    const size_t at = mesh.indices[i + k] * 3;
                    const double weight = corner == k ? 0.6 : 0.2;
                    x += weight * mesh.points[at];
                    y += weight * mesh.points[at + 1];
                }
                EXPECT_TRUE (EvenOddAt (fixture.chains, x, y));
            }
        }
        std::vector<StorySliceFillVertex> legacy;
        BuildSliceFill (fixture.chains, 0, legacy);
        RecordProperty (std::string (fixture.name) + "_legacyVertices", int (legacy.size ()));
        RecordProperty (std::string (fixture.name) + "_indexedVertices", int (mesh.points.size () / 3));
        RecordProperty (std::string (fixture.name) + "_indexedTriangles", int (mesh.indices.size () / 3));
    }
}

TEST (IndexedSliceFill, ConcaveSharedEdgesHaveNoBandTJunctions)
{
    const auto chain = Ring ({ 0, 0, 6, 0, 6, 5, 5, 4, 5, 2, 3, 2, 3, 4, 1, 5, 0, 4 });
    StorySliceFillMesh mesh;
    std::string error;
    ASSERT_TRUE (BuildIndexedSliceFill ({ chain }, 0, mesh, error)) << error;
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (size_t i = 0; i < mesh.indices.size (); i += 3)
        for (size_t k = 0; k < 3; ++k) {
            auto a = mesh.indices[i + k], b = mesh.indices[i + (k + 1) % 3];
            ++edges[std::minmax (a, b)];
        }
    size_t boundary = 0, shared = 0;
    for (const auto& [edge, uses] : edges) {
        EXPECT_TRUE (uses == 1 || uses == 2);
        boundary += uses == 1;
        shared += uses == 2;
        const size_t a = 3 * edge.first, b = 3 * edge.second;
        const double dx = mesh.points[b] - mesh.points[a], dy = mesh.points[b + 1] - mesh.points[a + 1];
        for (size_t p = 0; p < mesh.points.size (); p += 3) {
            const double px = mesh.points[p] - mesh.points[a], py = mesh.points[p + 1] - mesh.points[a + 1];
            const double dot = px * dx + py * dy;
            EXPECT_FALSE (std::abs (px * dy - py * dx) < 1e-10 && dot > 1e-10 && dot < dx * dx + dy * dy - 1e-10);
        }
    }
    EXPECT_EQ (boundary, chain.Count ());
    EXPECT_EQ (shared, chain.Count () - 3);
    EXPECT_EQ (mesh.points.size () / 3, chain.Count ());
    std::vector<StorySliceFillVertex> legacy;
    BuildSliceFill ({ chain }, 0, legacy);
    EXPECT_LT (mesh.indices.size (), legacy.size ());
}

TEST (IndexedSliceFill, BothWindingsClosureAndSurveyCoordinatesPreserveSmallDetails)
{
    for (bool reverse : { false, true }) {
        auto chain = Ring ({ 0, 0, 0.25, 0, 1, 0, 1, 0.125, 0.25, 0.125, 0.25, 1, 0, 1, 0, 0 });
        std::vector<std::pair<double, double>> points;
        for (size_t i = 0; i < chain.Count (); ++i)
            points.emplace_back (chain.xy[2 * i] + 600000, chain.xy[2 * i + 1] + 6500000);
        if (reverse)
            std::reverse (points.begin (), points.end ());
        chain.xy.clear ();
        for (auto [x, y] : points)
            chain.xy.insert (chain.xy.end (), { x, y });
        StorySliceFillMesh mesh;
        std::string error;
        ASSERT_TRUE (BuildIndexedSliceFill ({ chain }, 123.456789, mesh, error)) << error;
        EXPECT_NEAR (IndexedArea (mesh), 0.34375, 1e-9);
        RecordProperty ("legacySurveyArea", std::to_string (FillArea ({ chain })));
        RecordProperty ("indexedSurveyArea", std::to_string (IndexedArea (mesh)));
        EXPECT_NEAR (UnionArea ({ chain }), 0.34375, 1e-9);
        for (size_t i = 2; i < mesh.points.size (); i += 3)
            EXPECT_DOUBLE_EQ (mesh.points[i], 123.456789);
    }
}

TEST (IndexedSliceFill, InvalidInputsRefuseAtomicallyAndEmptyDomainsReplace)
{
    const auto square = Ring ({ 0, 0, 1, 0, 1, 1, 0, 1 });
    const double nan = std::numeric_limits<double>::quiet_NaN ();
    for (auto chain : { Ring ({ 0, 0, 1, 0, 1 }), Ring ({ 0, 0, 1, 0, nan, 1 }), Ring ({ 0, 0, 1e10, 0, 0, 1 }) }) {
        StorySliceFillMesh mesh { { 1, 2, 3 }, { 0 } };
        std::string error;
        ASSERT_FALSE (BuildIndexedSliceFill ({ square, chain }, 0, mesh, error));
        EXPECT_FALSE (error.empty ());
        EXPECT_EQ (mesh.points, (std::vector<double> { 1, 2, 3 }));
        EXPECT_EQ (mesh.indices, (std::vector<uint32_t> { 0 }));
    }
    StorySliceFillMesh mesh { { 1, 2, 3 }, { 0 } };
    std::string error;
    EXPECT_FALSE (BuildIndexedSliceFill ({ square }, nan, mesh, error));
    EXPECT_EQ (mesh.points, (std::vector<double> { 1, 2, 3 }));
    auto open = square;
    open.closed = false;
    for (const auto& chains :
         std::vector<std::vector<SliceChain>> { {}, { open }, { Ring ({ 0, 0, 1, 1 }) }, { square, square } }) {
        ASSERT_TRUE (BuildIndexedSliceFill (chains, 0, mesh, error)) << error;
        EXPECT_TRUE (mesh.points.empty ());
        EXPECT_TRUE (mesh.indices.empty ());
        EXPECT_TRUE (error.empty ());
    }
}

TEST (IndexedSliceFill, CrossingChainsAreNormalizedBeforeNonZeroTriangulation)
{
    const auto bow = Ring ({ 0, 0, 4, 4, 0, 4, 4, 0 });
    StorySliceFillMesh mesh;
    std::string error;
    ASSERT_TRUE (BuildIndexedSliceFill ({ bow }, 0, mesh, error)) << error;
    EXPECT_NEAR (IndexedArea (mesh), 8, 1e-6);
    bool crossing = false;
    for (size_t i = 0; i < mesh.points.size (); i += 3)
        crossing = crossing || (mesh.points[i] == 2 && mesh.points[i + 1] == 2);
    EXPECT_TRUE (crossing) << "normalization owns generated intersection vertices";
}

TEST (IndexedSliceFill, ManyHolesRetainTheConstrainedAreaCheckedCover)
{
    std::vector<SliceChain> chains { Ring ({ 0, 0, 40, 0, 40, 40, 0, 40 }) };
    for (int i = 0; i < 128; ++i) {
        const double x = 1 + (i % 16) * 2, y = 1 + (i / 16) * 2;
        chains.push_back (Ring ({ x, y, x + 1, y, x + 1, y + 1, x, y + 1 }));
    }
    StorySliceFillMesh mesh;
    std::string error;
    ASSERT_TRUE (BuildIndexedSliceFill (chains, 0, mesh, error)) << error;
    EXPECT_NEAR (IndexedArea (mesh), 1472, 1e-6);
}

TEST (IndexedSliceFill, ExplicitNormalizationPrecisionRetainsFacadeGridAndRefusesInvalidOptions)
{
    const double width = 1 + std::ldexp (1.0, -23);
    const auto chain = Ring ({ 0, 0, width, 0, width, 1, 0, 1 });
    StorySliceFillMesh mesh;
    std::string error;
    ASSERT_TRUE (BuildIndexedSliceFill ({ chain }, 0, mesh, error, 7)) << error;
    double area = -1;
    ASSERT_TRUE (geomsrv::archviz::MeasureSliceFill (mesh, area, error)) << error;
    EXPECT_DOUBLE_EQ (area, width);
    ASSERT_TRUE (BuildIndexedSliceFill ({ chain }, 0, mesh, error, 6)) << error;
    ASSERT_TRUE (geomsrv::archviz::MeasureSliceFill (mesh, area, error)) << error;
    EXPECT_DOUBLE_EQ (area, 1);
    const auto before = mesh.points;
    for (int precision : { -1, 9 }) {
        EXPECT_FALSE (BuildIndexedSliceFill ({ chain }, 0, mesh, error, precision));
        EXPECT_EQ (mesh.points, before);
        EXPECT_FALSE (error.empty ());
    }
}

TEST (IndexedSliceFill, MeasurementRefusesCorruptMeshesAtomicallyAndUnionDoesNotInventZero)
{
    StorySliceFillMesh valid;
    std::string error;
    ASSERT_TRUE (BuildIndexedSliceFill ({ Ring ({ 0, 0, 1, 0, 1, 1, 0, 1 }) }, 0, valid, error));
    for (int fault = 0; fault < 5; ++fault) {
        auto bad = valid;
        if (fault == 0)
            bad.points.pop_back ();
        if (fault == 1)
            bad.indices.pop_back ();
        if (fault == 2)
            bad.indices[0] = 999;
        if (fault == 3)
            bad.points[0] = std::numeric_limits<double>::infinity ();
        if (fault == 4)
            std::swap (bad.indices[0], bad.indices[1]);
        double area = 123;
        EXPECT_FALSE (geomsrv::archviz::MeasureSliceFill (bad, area, error));
        EXPECT_EQ (area, 123);
        EXPECT_FALSE (error.empty ());
    }
    double area = 123;
    EXPECT_TRUE (geomsrv::archviz::MeasureSliceFill ({}, area, error));
    EXPECT_EQ (area, 0);
    EXPECT_TRUE (std::isnan (UnionArea ({ Ring ({ 0, 0, 1, 0, 1 }) })));
}
