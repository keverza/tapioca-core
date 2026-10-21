#include "Edit/EditPlanar.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

namespace geo = tapioca::geometry;
using geo::PlanarCurve;
using geo::PlanarDiagnosticCode;
using geo::PlanarFace;
using geo::PlanarMap;
using geo::PlanarOptions;
using geo::Vec3;

namespace {

constexpr double kPi = 3.14159265358979323846;

Vec3 P (double x, double y)
{
    return { x, y, 0.0 };
}

PlanarCurve Closed (geo::Id source, std::initializer_list<Vec3> points)
{
    PlanarCurve curve;
    curve.source = source;
    curve.points.assign (points.begin (), points.end ());
    curve.closed = true;
    return curve;
}

PlanarCurve Open (geo::Id source, std::initializer_list<Vec3> points)
{
    PlanarCurve curve;
    curve.source = source;
    curve.points.assign (points.begin (), points.end ());
    return curve;
}

PlanarCurve RegularPolygon (geo::Id source, Vec3 centre, double radius, std::size_t count, double phase)
{
    PlanarCurve curve;
    curve.source = source;
    curve.closed = true;
    for (std::size_t i = 0; i < count; ++i) {
        const double angle = phase + 2.0 * kPi * static_cast<double> (i) / static_cast<double> (count);
        curve.points.push_back ({ centre.x + radius * std::cos (angle), centre.y + radius * std::sin (angle), 0.0 });
    }
    return curve;
}

bool HasDiagnostic (const PlanarMap& map, PlanarDiagnosticCode code)
{
    return std::any_of (map.diagnostics.begin (), map.diagnostics.end (),
                        [&] (const geo::PlanarDiagnostic& diagnostic) { return diagnostic.code == code; });
}

const PlanarFace* FaceNear (const PlanarMap& map, double area)
{
    for (const auto& face : map.faces)
        if (std::abs (face.area - area) <= 1.0e-7)
            return &face;
    return nullptr;
}

double QuantizedArea (const geo::FaceCycle& cycle)
{
    if (cycle.x.size () < 3)
        return 0.0;
    long double twice = 0.0L;
    for (std::size_t i = 0; i < cycle.x.size (); ++i) {
        const std::size_t j = (i + 1) % cycle.x.size ();
        twice += static_cast<long double> (cycle.x[i]) * static_cast<long double> (cycle.y[j]) -
                 static_cast<long double> (cycle.x[j]) * static_cast<long double> (cycle.y[i]);
    }
    return static_cast<double> (twice * 0.5L);
}

void ExpectBuild (const std::vector<PlanarCurve>& curves, const PlanarOptions& options, PlanarMap& map)
{
    std::string error;
    ASSERT_TRUE (geo::BuildPlanarMap (curves, options, map, error)) << error;
}

} // namespace

TEST (EditPlanar, MultipleDanglingGuidesDoNotCloseBoundaryFragmentsWithChords)
{
    const auto square = Closed (1, { P (0, 0), P (4, 0), P (4, 4), P (0, 4) });
    for (const double length : { -1.0, 1.0 }) {
        PlanarMap map;
        std::string error;
        ASSERT_TRUE (geo::BuildPlanarMap (
            { square, Open (2, { P (0, 1), P (length, 1) }), Open (3, { P (4, 3), P (4 - length, 3) }) }, {}, map,
            error))
            << error;
        ASSERT_EQ (map.faces.size (), 1u) << "Two spurs must not create a new face.";
        EXPECT_NEAR (map.faces.front ().area, 16, 1e-10);
        EXPECT_EQ (map.faces.front ().halfEdges.size (), 6u);
        EXPECT_TRUE (map.faces.front ().holes.empty ());
    }
}

TEST (EditPlanar, EmptyInputIsAValidEmptyMap)
{
    PlanarMap map;
    std::string error;
    ASSERT_TRUE (geo::BuildPlanarMap ({}, PlanarOptions {}, map, error)) << error;
    EXPECT_TRUE (map.vertices.empty ());
    EXPECT_TRUE (map.edges.empty ());
    EXPECT_TRUE (map.halfEdges.empty ());
    EXPECT_TRUE (map.faces.empty ());
    EXPECT_TRUE (map.diagnostics.empty ());
}

TEST (EditPlanar, SingleClosedRingHasOneFaceAndProvenance)
{
    PlanarMap map;
    ExpectBuild ({ Closed (10, { P (0, 0), P (10, 0), P (10, 10), P (0, 10) }) }, PlanarOptions {}, map);
    ASSERT_EQ (map.faces.size (), 1u);
    const PlanarFace& face = map.faces.front ();
    EXPECT_NEAR (face.area, 100.0, 1.0e-9);
    EXPECT_EQ (face.id, 1u);
    EXPECT_FALSE (face.descriptor.outer.hole);
    ASSERT_EQ (face.descriptor.outer.x.size (), 4u);
    EXPECT_GT (QuantizedArea (face.descriptor.outer), 0.0);
    EXPECT_FALSE (face.descriptor.outer.x.front () == face.descriptor.outer.x.back () &&
                  face.descriptor.outer.y.front () == face.descriptor.outer.y.back ());
    EXPECT_EQ (face.sources, (std::vector<geo::Id> { 10 }));
    EXPECT_EQ (face.descriptor.sourcePieces.size (), 4u);
    for (const auto& piece : face.descriptor.sourcePieces)
        EXPECT_EQ (piece.source, 10u);
    EXPECT_GT (face.seed.x, 0.0);
    EXPECT_LT (face.seed.x, 10.0);
    EXPECT_GT (face.seed.y, 0.0);
    EXPECT_LT (face.seed.y, 10.0);
}

TEST (EditPlanar, TwoDisconnectedRingsProduceTwoFaces)
{
    PlanarMap map;
    ExpectBuild ({ Closed (1, { P (0, 0), P (10, 0), P (10, 10), P (0, 10) }),
                   Closed (2, { P (20, 0), P (30, 0), P (30, 10), P (20, 10) }) },
                 PlanarOptions {}, map);
    ASSERT_EQ (map.faces.size (), 2u);
    EXPECT_NEAR (map.faces[0].area, 100.0, 1.0e-9);
    EXPECT_NEAR (map.faces[1].area, 100.0, 1.0e-9);
    EXPECT_NE (map.faces[0].descriptor, map.faces[1].descriptor);
    EXPECT_EQ (map.faces[0].id, 1u);
    EXPECT_EQ (map.faces[1].id, 2u);
    EXPECT_TRUE (geo::FaceDescriptorLess (map.faces[0].descriptor, map.faces[1].descriptor));
}

TEST (EditPlanar, NestedRingsProduceHoleAndIsland)
{
    PlanarMap map;
    ExpectBuild ({ Closed (1, { P (0, 0), P (10, 0), P (10, 10), P (0, 10) }),
                   Closed (2, { P (3, 3), P (7, 3), P (7, 7), P (3, 7) }) },
                 PlanarOptions {}, map);
    ASSERT_EQ (map.faces.size (), 2u);
    const PlanarFace* outer = FaceNear (map, 84.0);
    const PlanarFace* island = FaceNear (map, 16.0);
    ASSERT_NE (outer, nullptr);
    ASSERT_NE (island, nullptr);
    ASSERT_EQ (outer->holes.size (), 1u);
    ASSERT_EQ (outer->descriptor.holes.size (), 1u);
    EXPECT_TRUE (outer->descriptor.holes.front ().hole);
    EXPECT_LT (QuantizedArea (outer->descriptor.holes.front ()), 0.0);
    EXPECT_EQ (outer->sources, (std::vector<geo::Id> { 1, 2 }));
    EXPECT_EQ (island->sources, (std::vector<geo::Id> { 2 }));
    EXPECT_TRUE (island->holes.empty ());
    EXPECT_NEAR (outer->area + island->area, 100.0, 1.0e-9);
}

TEST (EditPlanar, ThreeNestedRingsKeepHoleHierarchy)
{
    PlanarMap map;
    ExpectBuild ({ Closed (1, { P (0, 0), P (30, 0), P (30, 30), P (0, 30) }),
                   Closed (2, { P (10, 10), P (20, 10), P (20, 20), P (10, 20) }),
                   Closed (3, { P (12, 12), P (18, 12), P (18, 18), P (12, 18) }) },
                 PlanarOptions {}, map);
    ASSERT_EQ (map.faces.size (), 3u);
    const PlanarFace* outer = FaceNear (map, 800.0);
    const PlanarFace* middle = FaceNear (map, 64.0);
    const PlanarFace* inner = FaceNear (map, 36.0);
    ASSERT_NE (outer, nullptr);
    ASSERT_NE (middle, nullptr);
    ASSERT_NE (inner, nullptr);
    EXPECT_EQ (outer->holes.size (), 1u);
    EXPECT_EQ (middle->holes.size (), 1u);
    EXPECT_TRUE (inner->holes.empty ());
    EXPECT_EQ (outer->sources, (std::vector<geo::Id> { 1, 2 }));
    EXPECT_EQ (middle->sources, (std::vector<geo::Id> { 2, 3 }));
    EXPECT_EQ (inner->sources, (std::vector<geo::Id> { 3 }));
}

TEST (EditPlanar, ClosedRingWithRepeatedFirstPointIsAccepted)
{
    PlanarMap map;
    ExpectBuild ({ Closed (1, { P (0, 0), P (10, 0), P (10, 10), P (0, 10), P (0, 0) }) }, PlanarOptions {}, map);
    ASSERT_EQ (map.faces.size (), 1u);
    EXPECT_NEAR (map.faces.front ().area, 100.0, 1.0e-9);
    EXPECT_EQ (map.faces.front ().descriptor.sourcePieces.size (), 4u);
    for (const auto& piece : map.faces.front ().descriptor.sourcePieces)
        EXPECT_LT (piece.piece, 4u);
}

TEST (EditPlanar, TwoHolesAreSortedOnTheFaceDescriptor)
{
    PlanarMap map;
    ExpectBuild ({ Closed (1, { P (0, 0), P (20, 0), P (20, 20), P (0, 20) }),
                   Closed (2, { P (2, 2), P (4, 2), P (4, 4), P (2, 4) }),
                   Closed (3, { P (10, 10), P (12, 10), P (12, 12), P (10, 12) }) },
                 PlanarOptions {}, map);
    ASSERT_EQ (map.faces.size (), 3u);
    const PlanarFace* outer = FaceNear (map, 392.0);
    ASSERT_NE (outer, nullptr);
    ASSERT_EQ (outer->holes.size (), 2u);
    ASSERT_EQ (outer->descriptor.holes.size (), 2u);
    EXPECT_TRUE (geo::FaceCycleLess (outer->descriptor.holes[0], outer->descriptor.holes[1]));
    EXPECT_EQ (outer->sources, (std::vector<geo::Id> { 1, 2, 3 }));
    int islands = 0;
    for (const auto& face : map.faces)
        if (std::abs (face.area - 4.0) <= 1.0e-9)
            ++islands;
    EXPECT_EQ (islands, 2);
}

TEST (EditPlanar, LineAcrossRectangleSplitsIt)
{
    PlanarMap map;
    ExpectBuild ({ Closed (1, { P (0, 0), P (10, 0), P (10, 10), P (0, 10) }), Open (2, { P (5, -2), P (5, 12) }) },
                 PlanarOptions {}, map);
    ASSERT_EQ (map.faces.size (), 2u);
    EXPECT_NEAR (map.faces[0].area, 50.0, 1.0e-9);
    EXPECT_NEAR (map.faces[1].area, 50.0, 1.0e-9);
    EXPECT_EQ (map.faces[0].sources, (std::vector<geo::Id> { 1, 2 }));
    EXPECT_EQ (map.faces[1].sources, (std::vector<geo::Id> { 1, 2 }));
    EXPECT_NE (map.faces[0].descriptor, map.faces[1].descriptor);
}

TEST (EditPlanar, ThreeCrossingOpenLinesFormATriangle)
{
    PlanarMap map;
    ExpectBuild ({ Open (1, { P (-2, 0), P (12, 0) }), Open (2, { P (0, -2), P (0, 12) }),
                   Open (3, { P (-2, 12), P (12, -2) }) },
                 PlanarOptions {}, map);
    ASSERT_EQ (map.faces.size (), 1u);
    EXPECT_NEAR (map.faces.front ().area, 50.0, 1.0e-9);
    EXPECT_EQ (map.faces.front ().sources, (std::vector<geo::Id> { 1, 2, 3 }));
}

TEST (EditPlanar, TangentCirclesRemainTwoFaces)
{
    const std::size_t count = 64;
    const double expected = 0.5 * static_cast<double> (count) * std::sin (2.0 * kPi / static_cast<double> (count));
    PlanarMap map;
    ExpectBuild ({ RegularPolygon (1, P (-1, 0), 1.0, count, 0.0), RegularPolygon (2, P (1, 0), 1.0, count, kPi) },
                 PlanarOptions {}, map);
    ASSERT_EQ (map.faces.size (), 2u);
    EXPECT_NEAR (map.faces[0].area, expected, 1.0e-9);
    EXPECT_NEAR (map.faces[1].area, expected, 1.0e-9);
    for (const auto& face : map.faces)
        EXPECT_EQ (face.sources.size (), 1u);
    EXPECT_TRUE (HasDiagnostic (map, PlanarDiagnosticCode::Tangent));
}

TEST (EditPlanar, NearCoincidentEdgesAreReportedWithoutHealing)
{
    PlanarMap map;
    ExpectBuild ({ Open (1, { P (0, 0), P (5, 0) }), Open (2, { P (2, 1.0e-7), P (7, 1.0e-7) }) }, PlanarOptions {},
                 map);
    EXPECT_TRUE (map.faces.empty ());
    EXPECT_TRUE (HasDiagnostic (map, PlanarDiagnosticCode::NearCoincident));
    EXPECT_TRUE (HasDiagnostic (map, PlanarDiagnosticCode::Unbounded));
}

TEST (EditPlanar, TinyGapIsNotHealedAtZeroAndIsHealedWithTolerance)
{
    const std::vector<PlanarCurve> curves { Open (1, { P (0, 0), P (10, 0) }), Open (2, { P (10, 0), P (10, 10) }),
                                            Open (3, { P (10, 10), P (0, 10) }),
                                            Open (4, { P (0, 10), P (0, 0.002) }) };
    PlanarOptions closedOptions;
    PlanarMap closed;
    ExpectBuild (curves, closedOptions, closed);
    EXPECT_TRUE (closed.faces.empty ());
    EXPECT_TRUE (HasDiagnostic (closed, PlanarDiagnosticCode::Unbounded));
    EXPECT_FALSE (HasDiagnostic (closed, PlanarDiagnosticCode::OpenGap));

    PlanarOptions healedOptions;
    healedOptions.gapTolerance = 0.003;
    PlanarMap healed;
    ExpectBuild (curves, healedOptions, healed);
    ASSERT_EQ (healed.faces.size (), 1u);
    EXPECT_NEAR (healed.faces.front ().area, 100.0, 1.0e-9);
    EXPECT_TRUE (HasDiagnostic (healed, PlanarDiagnosticCode::OpenGap));
}

TEST (EditPlanar, BowTieIsSplitIntoTwoTriangles)
{
    PlanarMap map;
    ExpectBuild ({ Closed (1, { P (0, 0), P (10, 10), P (0, 10), P (10, 0) }) }, PlanarOptions {}, map);
    ASSERT_EQ (map.faces.size (), 2u);
    EXPECT_NEAR (map.faces[0].area, 25.0, 1.0e-9);
    EXPECT_NEAR (map.faces[1].area, 25.0, 1.0e-9);
    EXPECT_TRUE (HasDiagnostic (map, PlanarDiagnosticCode::SelfIntersection));
}

TEST (EditPlanar, SliverBelowMinAreaIsDroppedWithDiagnostic)
{
    PlanarOptions options;
    options.snapTolerance = 1.0e-9;
    options.minArea = 1.0e-6;
    PlanarMap map;
    ExpectBuild ({ Closed (1, { P (0, 0), P (10, 0), P (10, 1.0e-8), P (0, 1.0e-8) }) }, options, map);
    EXPECT_TRUE (map.faces.empty ());
    EXPECT_TRUE (HasDiagnostic (map, PlanarDiagnosticCode::Sliver));
    EXPECT_TRUE (HasDiagnostic (map, PlanarDiagnosticCode::Unbounded));
}

TEST (EditPlanar, DeadEndGuideIsASpurNotAFaceBoundary)
{
    PlanarMap map;
    ExpectBuild ({ Closed (1, { P (0, 0), P (10, 0), P (10, 10), P (0, 10) }), Open (2, { P (5, 15), P (5, 5) }) },
                 PlanarOptions {}, map);
    ASSERT_EQ (map.faces.size (), 1u);
    EXPECT_NEAR (map.faces.front ().area, 100.0, 1.0e-9);
    EXPECT_EQ (map.faces.front ().sources, (std::vector<geo::Id> { 1 }));
    EXPECT_EQ (map.faces.front ().descriptor.outer.x.size (), 5u);
    const auto& halfEdges = map.faces.front ().halfEdges;
    for (std::size_t i = 0; i < halfEdges.size (); ++i)
        EXPECT_EQ (map.halfEdges[halfEdges[i]].next, halfEdges[(i + 1) % halfEdges.size ()]);
    std::vector<int> incoming (map.halfEdges.size (), 0);
    for (const auto& halfEdge : map.halfEdges)
        ++incoming[halfEdge.next];
    for (int count : incoming)
        EXPECT_EQ (count, 1);
}

TEST (EditPlanar, PartialCollinearOverlapKeepsBothSourcesOnTheSharedPiece)
{
    PlanarMap map;
    ExpectBuild ({ Open (1, { P (0, 0), P (10, 0) }), Open (2, { P (5, 0), P (15, 0) }) }, PlanarOptions {}, map);
    EXPECT_TRUE (map.faces.empty ());
    ASSERT_EQ (map.vertices.size (), 4u);
    bool shared = false;
    for (const auto& edge : map.edges) {
        if (edge.sources.size () == 2u) {
            shared = true;
            EXPECT_EQ (edge.sources, (std::vector<geo::Id> { 1, 2 }));
            EXPECT_EQ (edge.pieces.size (), 2u);
            EXPECT_NEAR (std::min (map.vertices[edge.a].x, map.vertices[edge.b].x), 5.0, 1.0e-12);
            EXPECT_NEAR (std::max (map.vertices[edge.a].x, map.vertices[edge.b].x), 10.0, 1.0e-12);
        }
    }
    EXPECT_TRUE (shared);
}

TEST (EditPlanar, CoincidentReversedCurvesShareOneEdgeAndProvenance)
{
    PlanarMap map;
    ExpectBuild ({ Closed (1, { P (0, 0), P (10, 0), P (10, 10), P (0, 10) }),
                   Closed (2, { P (0, 10), P (10, 10), P (10, 0), P (0, 0) }) },
                 PlanarOptions {}, map);
    ASSERT_EQ (map.faces.size (), 1u);
    EXPECT_NEAR (map.faces.front ().area, 100.0, 1.0e-9);
    EXPECT_EQ (map.faces.front ().sources, (std::vector<geo::Id> { 1, 2 }));
    EXPECT_EQ (map.faces.front ().descriptor.sourcePieces.size (), 8u);
    bool found = false;
    for (const auto& edge : map.edges) {
        if (edge.sources.size () == 2u && edge.pieces.size () == 2u) {
            found = true;
            EXPECT_EQ (edge.source, 1u);
            EXPECT_NE (edge.pieces[0].reversed, edge.pieces[1].reversed);
            EXPECT_TRUE (edge.sources[0] == 1u && edge.sources[1] == 2u);
        }
    }
    EXPECT_TRUE (found);
}

TEST (EditPlanar, ArcFlattenedCircleHasAnalyticArea)
{
    PlanarOptions options;
    options.chordTolerance = 1.0e-5;
    PlanarMap map;
    PlanarCurve circle;
    circle.source = 1;
    circle.closed = true;
    circle.points = { P (0, 0), P (2, 0) };
    circle.arcAngles = { kPi, kPi };
    ExpectBuild ({ circle }, options, map);
    ASSERT_EQ (map.faces.size (), 1u);
    EXPECT_NEAR (map.faces.front ().area, kPi, 1.0e-3);
    EXPECT_GT (map.vertices.size (), 10u);

    for (double sign : { -1.0, 1.0 }) {
        circle.arcAngles = { sign * kPi, sign * kPi };
        PlanarMap mirrored;
        ExpectBuild ({ circle }, options, mirrored);
        ASSERT_EQ (mirrored.faces.size (), 1u);
        EXPECT_NEAR (mirrored.faces.front ().area, kPi, 1.0e-3);
    }
}

TEST (EditPlanar, MajorArcUsesAnalyticGeometryAndSagitta)
{
    PlanarOptions options;
    options.chordTolerance = 1.0e-4;
    const double sweep = 1.5 * kPi;
    const double radius = 1.0 / std::sin (0.75 * kPi);
    const double expected = 0.5 * radius * radius * (sweep - std::sin (sweep));
    for (double sign : { -1.0, 1.0 }) {
        PlanarCurve arc;
        arc.source = 1;
        arc.points = { P (0, 0), P (2, 0) };
        arc.arcAngles = { sign * sweep, 0.0 };
        PlanarCurve chord;
        chord.source = 2;
        chord.points = { P (2, 0), P (0, 0) };
        PlanarMap map;
        std::string error;
        ASSERT_TRUE (geo::BuildPlanarMap ({ arc, chord }, options, map, error)) << error;
        ASSERT_EQ (map.faces.size (), 1u);
        EXPECT_NEAR (map.faces.front ().area, expected, 0.01);
        double minY = std::numeric_limits<double>::infinity ();
        double maxY = -std::numeric_limits<double>::infinity ();
        for (const Vec3& vertex : map.vertices) {
            minY = std::min (minY, vertex.y);
            maxY = std::max (maxY, vertex.y);
        }
        // The minor arc's sagitta is about 0.414; the major arc reaches 2.414.
        if (sign > 0.0)
            EXPECT_LT (minY, -2.3);
        else
            EXPECT_GT (maxY, 2.3);
        EXPECT_GT (map.vertices.size (), 20u);
    }
}

TEST (EditPlanar, FullTurnAndAmbiguousArcAnglesAreRefused)
{
    for (double angle : { 2.0 * kPi, -2.0 * kPi, 2.0 * kPi - 1.0e-12 }) {
        PlanarCurve curve;
        curve.source = 1;
        curve.closed = true;
        curve.points = { P (0, 0), P (2, 0) };
        curve.arcAngles = { angle, 0.0 };
        PlanarMap map;
        std::string error;
        EXPECT_FALSE (geo::BuildPlanarMap ({ curve }, PlanarOptions {}, map, error));
        EXPECT_NE (error.find ("full turn"), std::string::npos) << error;
        EXPECT_TRUE (map.faces.empty ());
    }
}
