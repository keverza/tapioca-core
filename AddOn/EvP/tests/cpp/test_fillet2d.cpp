#include "Geometry/Fillet2D.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>

namespace f = geomsrv::fillet2d;
namespace {
constexpr double kPi = 3.14159265358979323846;
double Distance (f::Point a, f::Point b)
{
    return std::hypot (a.x - b.x, a.y - b.y);
}
double Area (const std::vector<f::Point>& ring)
{
    double a = 0;
    for (size_t i = 0; i < ring.size (); ++i) {
        const auto p = ring[i], q = ring[(i + 1) % ring.size ()];
        a += p.x * q.y - p.y * q.x;
    }
    return a * .5;
}
f::InputRing Rectangle (double x, double y, double width, double height, double radius = 0)
{
    return { { { x, y }, { x + width, y }, { x + width, y + height }, { x, y + height } },
             std::vector<f::Request> (4, { radius }) };
}
const f::Corner& Corner (const f::Ring& ring, size_t source)
{
    return *std::find_if (ring.corners.begin (), ring.corners.end (),
                          [=] (const f::Corner& c) { return c.source == source; });
}
} // namespace

TEST (Fillet2D, ExactTangentsAndRadiusForFortyFiveNinetyAndOneThirtyFiveDegreeTurns)
{
    for (double degrees : { 45., 90., 135. }) {
        const double angle = degrees * kPi / 180;
        f::InputRing input { { { -10, 0 }, { 0, 0 }, { 10 * std::cos (angle), 10 * std::sin (angle) } },
                             { {}, { 1 }, {} } };
        f::Region out;
        std::string error;
        ASSERT_TRUE (f::Build ({ input }, {}, out, error)) << error;
        const auto& ring = out.rings[0];
        const auto arc = std::find_if (ring.edges.begin (), ring.edges.end (),
                                       [] (const f::Edge& e) { return e.kind == f::Kind::Arc; });
        ASSERT_NE (arc, ring.edges.end ());
        const double tangent = std::tan (angle * .5);
        EXPECT_NEAR (arc->from.x, -tangent, 1e-12);
        EXPECT_NEAR (arc->from.y, 0, 1e-12);
        EXPECT_NEAR (arc->to.x, tangent * std::cos (angle), 1e-12);
        EXPECT_NEAR (arc->to.y, tangent * std::sin (angle), 1e-12);
        EXPECT_NEAR (Distance (arc->from, arc->centre), 1, 1e-12);
        EXPECT_NEAR (Distance (arc->to, arc->centre), 1, 1e-12);
        EXPECT_NEAR (f::Tangent (*arc, 0).x, 1, 1e-12);
        EXPECT_NEAR (f::Tangent (*arc, 0).y, 0, 1e-12);
        EXPECT_NEAR (f::Tangent (*arc, 1).x, std::cos (angle), 1e-12);
        EXPECT_NEAR (f::Tangent (*arc, 1).y, std::sin (angle), 1e-12);
        EXPECT_NEAR (Corner (ring, 1).tangentDistance, tangent, 1e-12);
    }
}

TEST (Fillet2D, NeighbouringFilletsShareTheWholeAvailableEdgeRatherThanHalfEach)
{
    auto input = Rectangle (0, 0, 4, 20);
    input.corners[0].radius = .2;
    input.corners[1].radius = 3.5;
    f::Region out;
    std::string error;
    ASSERT_TRUE (f::Build ({ input }, {}, out, error)) << error;
    EXPECT_NEAR (Corner (out.rings[0], 1).radius, 3.5, 1e-12);
    input.corners[0].radius = input.corners[1].radius = 3;
    ASSERT_TRUE (f::Build ({ input }, {}, out, error)) << error;
    const auto& a = Corner (out.rings[0], 0);
    const auto& b = Corner (out.rings[0], 1);
    EXPECT_NEAR (a.radius, b.radius, 1e-12);
    EXPECT_LE (a.tangentDistance + b.tangentDistance, 4 - f::Settings {}.edgeClearance + 1e-12);
    EXPECT_DOUBLE_EQ (a.requestedRadius, 3);
    EXPECT_LT (a.radius, a.requestedRadius);
}

TEST (Fillet2D, ConvexConcaveAndHoleCornersRespectTheMaterialSide)
{
    f::Region region;
    std::string error;
    auto square = Rectangle (0, 0, 10, 10);
    square.corners[0].radius = 1;
    ASSERT_TRUE (f::Build ({ square }, {}, region, error)) << error;
    std::vector<std::vector<f::Point>> points;
    ASSERT_TRUE (f::Tessellate (region, {}, points, error)) << error;
    EXPECT_NEAR (Area (points[0]), 100 - (1 - kPi / 4), .005);
    f::InputRing recess { { { 0, 0 }, { 8, 0 }, { 8, 3 }, { 3, 3 }, { 3, 8 }, { 0, 8 } }, std::vector<f::Request> (6) };
    recess.corners[3].radius = 1;
    ASSERT_TRUE (f::Build ({ recess }, {}, region, error)) << error;
    ASSERT_TRUE (f::Tessellate (region, {}, points, error)) << error;
    EXPECT_NEAR (Area (points[0]), 39 + (1 - kPi / 4), .005);
    auto hole = Rectangle (3, 3, 4, 4, 1); // deliberately wrong winding: normalize by nesting
    ASSERT_TRUE (f::Build ({ Rectangle (0, 0, 10, 10), hole }, {}, region, error)) << error;
    EXPECT_FALSE (region.rings[0].hole);
    EXPECT_TRUE (region.rings[1].hole);
    ASSERT_TRUE (f::Tessellate (region, {}, points, error)) << error;
    EXPECT_GT (Area (points[0]), 0);
    EXPECT_LT (Area (points[1]), 0);
    EXPECT_NEAR (Area (points[0]) + Area (points[1]), 100 - 16 + 4 - kPi, .01);
}

TEST (Fillet2D, NonlocalHoleCollisionsAndNestingChangesReduceRadiiInsteadOfDeletingHoles)
{
    auto outside = Rectangle (0, 0, 10, 10);
    outside.corners[0].radius = 8;
    f::Region region;
    std::string error;
    const auto hole = Rectangle (.3, .3, .3, .3);
    ASSERT_TRUE (f::Build ({ outside, hole }, {}, region, error)) << error;
    ASSERT_EQ (region.rings.size (), 2u);
    EXPECT_TRUE (region.rings[1].hole);
    EXPECT_LE (Corner (region.rings[0], 0).radius, 1);
    EXPECT_DOUBLE_EQ (Corner (region.rings[0], 0).requestedRadius, 8);
    // A second placement intersects the analytic arc itself, rather than lying
    // wholly in the material a convex rounding would remove.
    ASSERT_TRUE (f::Build ({ outside, Rectangle (1, 3, 1, 1, .3) }, {}, region, error)) << error;
    EXPECT_LT (Corner (region.rings[0], 0).radius, 8);
}

TEST (Fillet2D, IslandsAndRepeatedRecomputationKeepTheirTopologyAndAuthoredIntent)
{
    const std::vector<f::InputRing> input { Rectangle (0, 0, 4, 4, 2), Rectangle (10, 0, 4, 4, 2) };
    f::Region first, second;
    std::string error;
    ASSERT_TRUE (f::Build (input, {}, first, error)) << error;
    ASSERT_TRUE (f::Build (input, {}, second, error)) << error;
    ASSERT_EQ (first.rings.size (), 2u);
    for (size_t r = 0; r < first.rings.size (); ++r) {
        EXPECT_FALSE (first.rings[r].hole);
        ASSERT_EQ (first.rings[r].edges.size (), second.rings[r].edges.size ());
        for (size_t i = 0; i < first.rings[r].edges.size (); ++i) {
            EXPECT_EQ (first.rings[r].edges[i].kind, second.rings[r].edges[i].kind);
            EXPECT_EQ (f::At (first.rings[r].edges[i], .5), f::At (second.rings[r].edges[i], .5));
        }
    }
    EXPECT_DOUBLE_EQ (input[0].corners[0].radius, 2);
}

TEST (Fillet2D, NestedHolesAndIslandsKeepTheirImmediateParents)
{
    const std::vector<f::InputRing> input { Rectangle (0, 0, 20, 20, .5), Rectangle (2, 2, 16, 16, .5),
                                            Rectangle (4, 4, 12, 12, .5), Rectangle (6, 6, 8, 8, .5) };
    f::Region out;
    std::string error;
    ASSERT_TRUE (f::Build (input, {}, out, error)) << error;
    ASSERT_EQ (out.rings.size (), 4u);
    EXPECT_EQ (out.rings[0].parent, std::numeric_limits<size_t>::max ());
    for (size_t i = 1; i < 4; ++i) {
        EXPECT_EQ (out.rings[i].parent, i - 1);
        EXPECT_EQ (out.rings[i].hole, i % 2 != 0);
    }
}

TEST (Fillet2D, ChordErrorIsControlledAndBudgetFailuresAreAtomic)
{
    f::Region region;
    std::string error;
    ASSERT_TRUE (f::Build ({ Rectangle (0, 0, 10, 10, 2) }, {}, region, error)) << error;
    std::vector<std::vector<f::Point>> sampled;
    ASSERT_TRUE (f::Tessellate (region, {}, sampled, error)) << error;
    for (const auto& edge : region.rings[0].edges)
        if (edge.kind == f::Kind::Arc) {
            f::Region arcOnly;
            arcOnly.rings.push_back ({ false, { edge }, {} });
            std::vector<std::vector<f::Point>> arc;
            ASSERT_TRUE (f::Tessellate (arcOnly, {}, arc, error));
            arc[0].push_back (edge.to);
            for (size_t i = 1; i < arc[0].size (); ++i) {
                const auto a = arc[0][i - 1], b = arc[0][i];
                const f::Point middle { (a.x + b.x) * .5, (a.y + b.y) * .5 };
                EXPECT_LE (edge.radius - Distance (edge.centre, middle), f::Settings {}.chordError + 1e-12);
            }
        }
    const auto saved = sampled;
    f::Settings limited;
    limited.maxArcSegments = 1;
    EXPECT_FALSE (f::Tessellate (region, limited, sampled, error));
    EXPECT_EQ (sampled, saved);
    const auto oldEdges = region.rings[0].edges.size ();
    const f::InputRing bowtie { { { 0, 0 }, { 4, 4 }, { 0, 4 }, { 4, 0 } }, {} };
    EXPECT_FALSE (f::Build ({ bowtie }, {}, region, error));
    EXPECT_EQ (region.rings[0].edges.size (), oldEdges);
}

TEST (Fillet2D, SampledCurvesWithNoSemanticCornerRequestsAreNotRoundedAgain)
{
    f::InputRing input;
    for (size_t i = 0; i < 64; ++i) {
        const double angle = 2 * kPi * i / 64;
        input.points.push_back ({ 10 * std::cos (angle), 10 * std::sin (angle) });
    }
    f::Region region;
    std::string error;
    ASSERT_TRUE (f::Build ({ input }, {}, region, error)) << error;
    ASSERT_EQ (region.rings[0].edges.size (), 64u);
    for (const auto& edge : region.rings[0].edges)
        EXPECT_EQ (edge.kind, f::Kind::Line);
}
