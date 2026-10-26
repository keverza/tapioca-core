#include "Geometry/PlanarTriangulation.hpp"

#include <clipper2/clipper.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace geo = geomsrv::engine;

using OwnedPolygon = std::vector<std::vector<double>>;

std::vector<geo::PlanarRing> Views (const OwnedPolygon& polygon)
{
    std::vector<geo::PlanarRing> rings;
    rings.reserve (polygon.size ());
    for (const auto& points : polygon)
        rings.push_back ({ points.data (), points.size () / 2 });
    return rings;
}

std::vector<double> Flatten (const OwnedPolygon& polygon)
{
    std::vector<double> flat;
    for (const auto& ring : polygon)
        flat.insert (flat.end (), ring.begin (), ring.end ());
    return flat;
}

double Cross (double ax, double ay, double bx, double by, double cx, double cy)
{
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

double SignedArea (const std::vector<double>& ring)
{
    double area = 0.0;
    for (size_t index = 0; index < ring.size (); index += 2) {
        const size_t next = (index + 2) % ring.size ();
        area += ring[index] * ring[next + 1] - ring[next] * ring[index + 1];
    }
    return area * 0.5;
}

double CoveredArea (const OwnedPolygon& polygon, const geo::PlanarTriangulation& result)
{
    std::vector<double> points = Flatten (polygon);
    points.insert (points.end (), result.generatedXY.begin (), result.generatedXY.end ());
    double area = 0.0;
    for (size_t index = 0; index < result.triangles.size (); index += 3) {
        const size_t a = static_cast<size_t> (result.triangles[index]) * 2;
        const size_t b = static_cast<size_t> (result.triangles[index + 1]) * 2;
        const size_t c = static_cast<size_t> (result.triangles[index + 2]) * 2;
        area += Cross (points[a], points[a + 1], points[b], points[b + 1], points[c], points[c + 1]) * 0.5;
    }
    return area;
}

void ExpectValidTriangles (const OwnedPolygon& polygon, const geo::PlanarTriangulation& result)
{
    ASSERT_FALSE (result.triangles.empty ());
    ASSERT_EQ (result.triangles.size () % 3, 0u);
    const size_t inputCount = Flatten (polygon).size () / 2;
    const size_t vertexCount = inputCount + result.generatedXY.size () / 2;
    const std::vector<double> points = [&] {
        std::vector<double> value = Flatten (polygon);
        value.insert (value.end (), result.generatedXY.begin (), result.generatedXY.end ());
        return value;
    }();
    for (size_t index = 0; index < result.triangles.size (); index += 3) {
        const uint32_t ia = result.triangles[index];
        const uint32_t ib = result.triangles[index + 1];
        const uint32_t ic = result.triangles[index + 2];
        ASSERT_LT (ia, vertexCount);
        ASSERT_LT (ib, vertexCount);
        ASSERT_LT (ic, vertexCount);
        const size_t a = static_cast<size_t> (ia) * 2;
        const size_t b = static_cast<size_t> (ib) * 2;
        const size_t c = static_cast<size_t> (ic) * 2;
        EXPECT_GT (Cross (points[a], points[a + 1], points[b], points[b + 1], points[c], points[c + 1]), 0.0);
    }
}

bool InsideOrBoundary (const std::vector<double>& ring, double x, double y)
{
    bool inside = false;
    const size_t count = ring.size () / 2;
    for (size_t index = 0; index < count; ++index) {
        const size_t next = (index + 1) % count;
        const double ax = ring[index * 2], ay = ring[index * 2 + 1];
        const double bx = ring[next * 2], by = ring[next * 2 + 1];
        const double dx = bx - ax, dy = by - ay;
        const double length2 = dx * dx + dy * dy;
        if (length2 > 0.0) {
            const double t = ((x - ax) * dx + (y - ay) * dy) / length2;
            if (t >= 0.0 && t <= 1.0) {
                const double ex = x - (ax + t * dx), ey = y - (ay + t * dy);
                if (ex * ex + ey * ey <= 1.0e-20)
                    return true;
            }
        }
        if ((ay > y) != (by > y)) {
            const double crossingX = ax + (y - ay) * dx / dy;
            if (crossingX > x)
                inside = !inside;
        }
    }
    return inside;
}

void ExpectTriangleSamplesInside (const OwnedPolygon& polygon, const geo::PlanarTriangulation& result)
{
    const std::vector<double> points = Flatten (polygon);
    for (size_t index = 0; index < result.triangles.size (); index += 3) {
        const size_t a = result.triangles[index] * 2;
        const size_t b = result.triangles[index + 1] * 2;
        const size_t c = result.triangles[index + 2] * 2;
        for (const auto& weight :
             { std::pair<double, double> { 0.6, 0.2 }, { 0.2, 0.6 }, { 0.2, 0.2 }, { 1.0 / 3.0, 1.0 / 3.0 } }) {
            const double x =
                weight.first * points[a] + weight.second * points[b] + (1.0 - weight.first - weight.second) * points[c];
            const double y = weight.first * points[a + 1] + weight.second * points[b + 1] +
                             (1.0 - weight.first - weight.second) * points[c + 1];
            ASSERT_TRUE (InsideOrBoundary (polygon[0], x, y));
            for (size_t hole = 1; hole < polygon.size (); ++hole)
                ASSERT_FALSE (InsideOrBoundary (polygon[hole], x, y));
        }
    }
}

void ExpectSameResult (const geo::PlanarTriangulation& actual, const geo::PlanarTriangulation& expected)
{
    EXPECT_EQ (actual.triangles, expected.triangles);
    EXPECT_EQ (actual.strips, expected.strips);
    EXPECT_EQ (actual.stripEnds, expected.stripEnds);
    EXPECT_EQ (actual.stripClockwise, expected.stripClockwise);
    EXPECT_EQ (actual.generatedXY, expected.generatedXY);
    EXPECT_EQ (actual.cover, expected.cover);
}

geo::PlanarTriangulation Sentinel ()
{
    geo::PlanarTriangulation result;
    result.triangles = { 70, 71, 72 };
    result.strips = { 80, 81, 82 };
    result.stripEnds = { 3 };
    result.stripClockwise = { 1 };
    result.generatedXY = { 90, 91 };
    result.cover = geo::PlanarCover::Approximate;
    return result;
}

void ExpectRefusalUnchanged (const std::vector<geo::PlanarRing>& rings)
{
    geo::PlanarTriangulation result = Sentinel ();
    const geo::PlanarTriangulation expected = result;
    std::string error;
    EXPECT_FALSE (geo::TriangulatePlanar (rings, true, result, error));
    EXPECT_FALSE (error.empty ());
    ExpectSameResult (result, expected);
}

uint64_t Digest (const geo::PlanarTriangulation& result)
{
    uint64_t hash = 14695981039346656037ULL;
    const auto byte = [&hash] (uint8_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    for (uint32_t index : result.triangles) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            byte (static_cast<uint8_t> (index >> shift));
    }
    byte (static_cast<uint8_t> (result.cover));
    const uint32_t generated = static_cast<uint32_t> (result.generatedXY.size () / 2);
    for (unsigned shift = 0; shift < 32; shift += 8)
        byte (static_cast<uint8_t> (generated >> shift));
    return hash;
}

} // namespace

TEST (PlanarTriangulation, SquareBothWindingsIsExactAndDeterministic)
{
    const OwnedPolygon ccw { { 0, 0, 1, 0, 1, 1, 0, 1 } };
    const OwnedPolygon cw { { 0, 1, 1, 1, 1, 0, 0, 0 } };
    EXPECT_GT (SignedArea (ccw[0]), 0.0);
    EXPECT_LT (SignedArea (cw[0]), 0.0);
    const std::vector<const OwnedPolygon*> polygons { &ccw, &cw };
    for (const auto* polygon : polygons) {
        const auto rings = Views (*polygon);
        geo::PlanarTriangulation first, second;
        std::string error;
        ASSERT_TRUE (geo::TriangulatePlanar (rings, false, first, error)) << error;
        EXPECT_EQ (first.cover, geo::PlanarCover::Exact);
        EXPECT_EQ (first.triangles.size (), 6u);
        EXPECT_NEAR (CoveredArea (*polygon, first), 1.0, 1.0e-12);
        EXPECT_TRUE (first.strips.empty ());
        ExpectValidTriangles (*polygon, first);
        ASSERT_TRUE (geo::TriangulatePlanar (rings, false, second, error)) << error;
        ExpectSameResult (second, first);
    }
}

TEST (PlanarTriangulation, ConcaveLStaysOutOfNotch)
{
    const OwnedPolygon polygon { { 0, 0, 4, 0, 4, 1, 1, 1, 1, 4, 0, 4 } };
    geo::PlanarTriangulation result;
    std::string error;
    ASSERT_TRUE (geo::TriangulatePlanar (Views (polygon), false, result, error)) << error;
    EXPECT_EQ (result.cover, geo::PlanarCover::Exact);
    EXPECT_NEAR (CoveredArea (polygon, result), 7.0, 1.0e-12);
    ExpectValidTriangles (polygon, result);
    ExpectTriangleSamplesInside (polygon, result);
}

TEST (PlanarTriangulation, UWithHoleAndClosedRingsPreservesDeclaredArea)
{
    const OwnedPolygon polygon {
        { 0, 0, 4, 0, 4, 4, 3, 4, 3, 1, 1, 1, 1, 4, 0, 4, 0, 0 },
        { 1.5, 0.25, 2.5, 0.25, 2.5, 0.75, 1.5, 0.75, 1.5, 0.25 },
    };
    geo::PlanarTriangulation result;
    std::string error;
    ASSERT_TRUE (geo::TriangulatePlanar (Views (polygon), false, result, error)) << error;
    EXPECT_EQ (result.cover, geo::PlanarCover::Exact);
    EXPECT_NEAR (CoveredArea (polygon, result), 9.5, 1.0e-12);
    ExpectValidTriangles (polygon, result);
    ExpectTriangleSamplesInside (polygon, result);
}

TEST (PlanarTriangulation, HoleTouchingOuterAtVertexIsBridged)
{
    const OwnedPolygon polygon {
        { 0, 0, 4, 0, 4, 4, 0, 4 },
        { 0, 0, 1, 0.5, 0.5, 1 },
    };
    geo::PlanarTriangulation result;
    std::string error;
    ASSERT_TRUE (geo::TriangulatePlanar (Views (polygon), false, result, error)) << error;
    EXPECT_EQ (result.cover, geo::PlanarCover::Exact);
    EXPECT_NEAR (CoveredArea (polygon, result), 16.0 - 0.375, 1.0e-12);
    ExpectValidTriangles (polygon, result);
}

TEST (PlanarTriangulation, OutsideAndCrossingHolesRefuseAtomically)
{
    const OwnedPolygon outside {
        { 0, 0, 10, 0, 10, 10, 0, 10 },
        { 20, 0, 24, 0, 24, 4, 20, 4 },
    };
    ExpectRefusalUnchanged (Views (outside));
    const OwnedPolygon crossing {
        { 0, 0, 10, 0, 10, 10, 0, 10 },
        { 2, -1, 3, -1, 3, 1, 2, 1 },
    };
    ExpectRefusalUnchanged (Views (crossing));
}

TEST (PlanarTriangulation, ZeroAreaSelfCrossingHoleRefusesAtomically)
{
    const OwnedPolygon polygon {
        { 0, 0, 10, 0, 10, 10, 0, 10 },
        { 2, 2, 8, 8, 8, 2, 2, 8 },
    };
    ExpectRefusalUnchanged (Views (polygon));
}

TEST (PlanarTriangulation, BowtieRepairExposesGeneratedCrossing)
{
    const OwnedPolygon polygon { { 0, 0, 10, 10, 10, 0, 0, 10 } };
    geo::PlanarTriangulation result, repeated;
    std::string error;
    ASSERT_TRUE (geo::TriangulatePlanar (Views (polygon), false, result, error)) << error;
    EXPECT_EQ (result.cover, geo::PlanarCover::Repaired);
    ASSERT_EQ (result.generatedXY.size (), 2u);
    EXPECT_DOUBLE_EQ (result.generatedXY[0], 5.0);
    EXPECT_DOUBLE_EQ (result.generatedXY[1], 5.0);
    EXPECT_NEAR (CoveredArea (polygon, result), 50.0, 1.0e-12);
    ExpectValidTriangles (polygon, result);
    ASSERT_TRUE (geo::TriangulatePlanar (Views (polygon), false, repeated, error)) << error;
    ExpectSameResult (repeated, result);
}

TEST (PlanarTriangulation, DoubledOuterUsesNonZeroRepair)
{
    const OwnedPolygon polygon { { 0, 0, 10, 0, 10, 10, 0, 10, 0, 0, 10, 0, 10, 10, 0, 10 } };
    geo::PlanarTriangulation result;
    std::string error;
    ASSERT_TRUE (geo::TriangulatePlanar (Views (polygon), false, result, error)) << error;
    EXPECT_EQ (result.cover, geo::PlanarCover::Repaired);
    EXPECT_TRUE (result.generatedXY.empty ());
    EXPECT_NEAR (CoveredArea (polygon, result), 100.0, 1.0e-12);
    ExpectValidTriangles (polygon, result);
}

TEST (PlanarTriangulation, DuplicateClosureAndCollinearEdgesEmitNoFlatTriangle)
{
    const OwnedPolygon closed { { 0, 0, 1, 0, 1, 1, 0, 1, 0, 0 } };
    const OwnedPolygon collinear { { 0, 0, 0.5, 0, 1, 0, 1, 1, 0, 1 } };
    for (const auto* polygon : { &closed, &collinear }) {
        geo::PlanarTriangulation result;
        std::string error;
        ASSERT_TRUE (geo::TriangulatePlanar (Views (*polygon), false, result, error)) << error;
        EXPECT_EQ (result.cover, geo::PlanarCover::Exact);
        EXPECT_NEAR (CoveredArea (*polygon, result), 1.0, 1.0e-12);
        ExpectValidTriangles (*polygon, result);
    }
}

TEST (PlanarTriangulation, MalformedAndDegenerateInputsRefuseAtomically)
{
    ExpectRefusalUnchanged ({});
    const OwnedPolygon shortRing { { 0, 0, 1, 1 } };
    ExpectRefusalUnchanged (Views (shortRing));
    const OwnedPolygon line { { 0, 0, 1, 0, 2, 0 } };
    ExpectRefusalUnchanged (Views (line));
    const OwnedPolygon collapsed { { 0, 0, 1, 0, 0, 0 } };
    ExpectRefusalUnchanged (Views (collapsed));
    const OwnedPolygon nonfinite { { 0, 0, 1, 0, std::numeric_limits<double>::quiet_NaN (), 1 } };
    ExpectRefusalUnchanged (Views (nonfinite));
    ExpectRefusalUnchanged ({ { nullptr, 3 } });
    const double point[] { 0, 0 };
    ExpectRefusalUnchanged ({ { point, std::numeric_limits<uint32_t>::max () } });
    const OwnedPolygon infinite { { 0, 0, 1, 0, 1, std::numeric_limits<double>::infinity () } };
    ExpectRefusalUnchanged (Views (infinite));
}

TEST (PlanarTriangulation, OverlappingHolesCannotSilentlyChangeArea)
{
    const OwnedPolygon polygon {
        { 0, 0, 10, 0, 10, 10, 0, 10 },
        { 1, 1, 5, 1, 5, 5, 1, 5 },
        { 3, 3, 7, 3, 7, 7, 3, 7 },
    };
    ExpectRefusalUnchanged (Views (polygon));
}

TEST (PlanarTriangulation, SurveyOffsetPreservesAreaAndRawIndices)
{
    const OwnedPolygon polygon { { 2600000, 1200000, 2600004, 1200000, 2600004, 1200001, 2600001, 1200001, 2600001,
                                   1200004, 2600000, 1200004 } };
    geo::PlanarTriangulation result;
    std::string error = "stale error";
    ASSERT_TRUE (geo::TriangulatePlanar (Views (polygon), false, result, error)) << error;
    EXPECT_TRUE (error.empty ());
    EXPECT_EQ (result.cover, geo::PlanarCover::Exact);
    EXPECT_NEAR (CoveredArea (polygon, result), 7.0, 1.0e-12);
    ExpectValidTriangles (polygon, result);
}

TEST (PlanarTriangulation, StripsMatchListAcrossScratchReuseAndShrink)
{
    const OwnedPolygon polygon {
        { 0, 0, 4, 0, 4, 4, 0, 4 },
        { 1, 1, 2, 1, 2, 2, 1, 2 },
    };
    geo::PlanarTriangulation strips, list, afterShrink;
    std::string error;
    ASSERT_TRUE (geo::TriangulatePlanar (Views (polygon), true, strips, error)) << error;
    ASSERT_TRUE (geo::TriangulatePlanar (Views (polygon), false, list, error)) << error;
    EXPECT_EQ (strips.triangles, list.triangles);
    EXPECT_FALSE (strips.strips.empty ());
    EXPECT_EQ (strips.stripEnds.size (), strips.stripClockwise.size ());
    std::vector<uint32_t> expanded;
    uint32_t begin = 0;
    for (size_t strip = 0; strip < strips.stripEnds.size (); ++strip) {
        const uint32_t end = strips.stripEnds[strip];
        ASSERT_LE (end, strips.strips.size ());
        ASSERT_GE (end, begin + 3);
        for (uint32_t index = begin; index + 2 < end; ++index) {
            const uint32_t flip = ((index - begin) & 1u) ^ strips.stripClockwise[strip];
            expanded.insert (expanded.end (), { strips.strips[index + flip], strips.strips[index + 1 - flip],
                                                strips.strips[index + 2] });
        }
        begin = end;
    }
    EXPECT_EQ (expanded, strips.triangles);
    EXPECT_TRUE (list.strips.empty ());
    geo::ShrinkPlanarTriangulationScratch ();
    ASSERT_TRUE (geo::TriangulatePlanar (Views (polygon), true, afterShrink, error)) << error;
    ExpectSameResult (afterShrink, strips);
}

TEST (PlanarTriangulation, MoreThan128RingsUsesConstrainedCover)
{
    OwnedPolygon polygon { { 0, 0, 33, 0, 33, 17, 0, 17 } };
    for (size_t row = 0; row < 8; ++row) {
        for (size_t column = 0; column < 16; ++column) {
            const double x = 1.0 + column * 2.0;
            const double y = 1.0 + row * 2.0;
            polygon.push_back ({ x, y, x + 0.5, y, x + 0.5, y + 0.5, x, y + 0.5 });
        }
    }
    ASSERT_EQ (polygon.size (), 129u);
    geo::PlanarTriangulation result;
    std::string error;
    ASSERT_TRUE (geo::TriangulatePlanar (Views (polygon), false, result, error)) << error;
    EXPECT_EQ (result.cover, geo::PlanarCover::Repaired);
    EXPECT_NEAR (CoveredArea (polygon, result), 529.0, 1.0e-9);
    ExpectValidTriangles (polygon, result);
}

TEST (PlanarTriangulation, FortyThousandVertexCombIsExact)
{
    constexpr size_t teeth = 9999;
    const double width = static_cast<double> (teeth * 4);
    OwnedPolygon polygon { { 0, 0, width, 0, width, 10 } };
    auto& comb = polygon[0];
    for (size_t tooth = teeth; tooth > 0; --tooth) {
        const double x = static_cast<double> (tooth * 4);
        comb.insert (comb.end (), { x - 1, 10, x - 1, 5, x - 3, 5, x - 3, 10 });
    }
    comb.insert (comb.end (), { 0, 10 });
    ASSERT_EQ (comb.size () / 2, 40000u);
    geo::PlanarTriangulation result;
    std::string error;
    ASSERT_TRUE (geo::TriangulatePlanar (Views (polygon), false, result, error)) << error;
    EXPECT_EQ (result.cover, geo::PlanarCover::Exact);
    EXPECT_NEAR (CoveredArea (polygon, result), 299970.0, 1.0e-8);
    ExpectValidTriangles (polygon, result);
}

TEST (PlanarTriangulation, ClipperDifferentialAgreesOnNormalizedRegions)
{
    const OwnedPolygon polygon {
        { 0, 0, 4, 0, 4, 4, 3, 4, 3, 1, 1, 1, 1, 4, 0, 4 },
        { 1.5, 0.25, 2.5, 0.25, 2.5, 0.75, 1.5, 0.75 },
    };
    Clipper2Lib::PathsD paths;
    for (size_t ringIndex = 0; ringIndex < polygon.size (); ++ringIndex) {
        const auto& ring = polygon[ringIndex];
        Clipper2Lib::PathD path;
        if (ringIndex == 0) {
            for (size_t index = 0; index < ring.size (); index += 2)
                path.emplace_back (ring[index], ring[index + 1]);
        }
        else {
            // Clipper2's differential input uses opposite hole winding;
            // earstrip itself accepts either winding.
            for (size_t index = ring.size (); index > 0; index -= 2)
                path.emplace_back (ring[index - 2], ring[index - 1]);
        }
        paths.push_back (std::move (path));
    }
    Clipper2Lib::PathsD triangles;
    ASSERT_EQ (Clipper2Lib::Triangulate (paths, 8, triangles), Clipper2Lib::TriangulateResult::success);
    double clipperArea = 0.0;
    for (const auto& triangle : triangles) {
        ASSERT_EQ (triangle.size (), 3u);
        clipperArea += std::abs (Cross (triangle[0].x, triangle[0].y, triangle[1].x, triangle[1].y, triangle[2].x,
                                        triangle[2].y)) *
                       0.5;
    }
    geo::PlanarTriangulation result;
    std::string error;
    ASSERT_TRUE (geo::TriangulatePlanar (Views (polygon), false, result, error)) << error;
    EXPECT_NE (result.cover, geo::PlanarCover::Refused);
    EXPECT_NEAR (CoveredArea (polygon, result), clipperArea, 1.0e-9);
}

TEST (PlanarTriangulation, FixtureDeterminismDigestsArePinned)
{
    struct Fixture {
        const char* name;
        OwnedPolygon polygon;
        uint64_t digest;
    };
    const std::vector<Fixture> fixtures {
        { "square", { { 0, 0, 1, 0, 1, 1, 0, 1 } }, 11919570676034427157ULL },
        { "L", { { 0, 0, 4, 0, 4, 1, 1, 1, 1, 4, 0, 4 } }, 10767600830250739993ULL },
        { "U-hole",
          { { 0, 0, 4, 0, 4, 4, 3, 4, 3, 1, 1, 1, 1, 4, 0, 4 }, { 1.5, 0.25, 2.5, 0.25, 2.5, 0.75, 1.5, 0.75 } },
          5507079602456319689ULL },
        { "bowtie", { { 0, 0, 10, 10, 10, 0, 0, 10 } }, 18259769967219396205ULL },
        { "closed-square", { { 0, 0, 1, 0, 1, 1, 0, 1, 0, 0 } }, 11919570676034427157ULL },
        { "collinear-square", { { 0, 0, 0.5, 0, 1, 0, 1, 1, 0, 1 } }, 1896688837819520178ULL },
    };
    for (const auto& fixture : fixtures) {
        SCOPED_TRACE (fixture.name);
        geo::PlanarTriangulation first, second;
        std::string error;
        ASSERT_TRUE (geo::TriangulatePlanar (Views (fixture.polygon), false, first, error)) << error;
        ASSERT_TRUE (geo::TriangulatePlanar (Views (fixture.polygon), false, second, error)) << error;
        ExpectSameResult (second, first);
        EXPECT_EQ (Digest (first), fixture.digest);
    }
}
