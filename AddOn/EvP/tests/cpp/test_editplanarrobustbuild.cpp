#include "Edit/EditPlanar.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace geo = tapioca::geometry;
using geo::PlanarCurve;
using geo::PlanarDiagnosticCode;
using geo::PlanarEdge;
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

bool HasDiagnostic (const PlanarMap& map, PlanarDiagnosticCode code)
{
    return std::any_of (map.diagnostics.begin (), map.diagnostics.end (),
                        [&] (const geo::PlanarDiagnostic& diagnostic) { return diagnostic.code == code; });
}

std::vector<uint32_t> PiecesOf (const PlanarFace& face)
{
    std::vector<uint32_t> pieces;
    for (const auto& piece : face.descriptor.sourcePieces)
        pieces.push_back (piece.piece);
    return pieces;
}

std::size_t CountPiece (const PlanarFace& face, uint32_t piece)
{
    return static_cast<std::size_t> (
        std::count_if (face.descriptor.sourcePieces.begin (), face.descriptor.sourcePieces.end (),
                       [piece] (const geo::FaceSourcePiece& sourcePiece) { return sourcePiece.piece == piece; }));
}

const PlanarEdge* BottomEdge (const PlanarMap& map)
{
    for (const auto& edge : map.edges) {
        if (std::abs (map.vertices[edge.a].y) <= 1.0e-12 && std::abs (map.vertices[edge.b].y) <= 1.0e-12)
            return &edge;
    }
    return nullptr;
}

void ExpectSameFaces (const PlanarMap& expected, const PlanarMap& actual)
{
    ASSERT_EQ (expected.faces.size (), actual.faces.size ());
    for (std::size_t i = 0; i < expected.faces.size (); ++i) {
        EXPECT_EQ (expected.faces[i].descriptor, actual.faces[i].descriptor);
        EXPECT_EQ (expected.faces[i].id, actual.faces[i].id);
        EXPECT_EQ (expected.faces[i].sources, actual.faces[i].sources);
        EXPECT_NEAR (expected.faces[i].area, actual.faces[i].area, 1.0e-9);
    }
}

PlanarCurve CubicArch ()
{
    PlanarCurve curve;
    curve.source = 1;
    curve.closed = true;
    curve.points = { P (0, 0), P (2, 0) };
    curve.inHandles = { Vec3 {}, P (0, 2) };
    curve.outHandles = { P (0, 2), Vec3 {} };
    return curve;
}

} // namespace

TEST (EditPlanarRobustBuild, ZeroWorkBudgetsAreRefused)
{
    const std::vector<PlanarCurve> curves { Closed (1, { P (0, 0), P (1, 0), P (1, 1) }) };
    PlanarMap map;
    std::string error;
    PlanarOptions noPairs;
    noPairs.maxCandidatePairs = 0;
    EXPECT_FALSE (geo::BuildPlanarMap (curves, noPairs, map, error));
    EXPECT_NE (error.find ("maxCandidatePairs"), std::string::npos) << error;
    PlanarOptions noSplits;
    noSplits.maxSplitEvents = 0;
    EXPECT_FALSE (geo::BuildPlanarMap (curves, noSplits, map, error));
    EXPECT_NE (error.find ("maxSplitEvents"), std::string::npos) << error;
    PlanarOptions noDepth;
    noDepth.maxSubdivisionDepth = 0;
    EXPECT_FALSE (geo::BuildPlanarMap (curves, noDepth, map, error));
    EXPECT_NE (error.find ("maxSubdivisionDepth"), std::string::npos) << error;
}

TEST (EditPlanarRobustBuild, CandidatePairBudgetRefusesAdversarialCoincidentLines)
{
    std::vector<PlanarCurve> curves;
    curves.reserve (20000);
    for (geo::Id source = 1; source <= 20000; ++source)
        curves.push_back (Open (source, { P (0, 0), P (1, 0) }));

    PlanarOptions options;
    options.maxCandidatePairs = 128;
    options.maxSplitEvents = 1000000;
    PlanarMap map;
    std::string error;
    EXPECT_FALSE (geo::BuildPlanarMap (curves, options, map, error));
    EXPECT_NE (error.find ("candidate-pair"), std::string::npos) << error;
    EXPECT_TRUE (HasDiagnostic (map, PlanarDiagnosticCode::WorkLimit));
    EXPECT_TRUE (map.faces.empty ());
}

TEST (EditPlanarRobustBuild, SplitEventBudgetIsCheckedSeparatelyFromPairs)
{
    std::vector<PlanarCurve> curves;
    curves.reserve (100);
    for (geo::Id source = 1; source <= 100; ++source)
        curves.push_back (Open (source, { P (0, 0), P (1, 0) }));

    PlanarOptions options;
    options.maxCandidatePairs = 10000; // 4950 pairs fit.
    options.maxSplitEvents = 8;
    PlanarMap map;
    std::string error;
    EXPECT_FALSE (geo::BuildPlanarMap (curves, options, map, error));
    EXPECT_NE (error.find ("split-event"), std::string::npos) << error;
    EXPECT_TRUE (HasDiagnostic (map, PlanarDiagnosticCode::WorkLimit));
    EXPECT_TRUE (map.faces.empty ());
}

TEST (EditPlanarRobustBuild, TinyChordToleranceRefusesRatherThanMissingTheFace)
{
    PlanarCurve circle;
    circle.source = 1;
    circle.closed = true;
    circle.points = { P (0, 0), P (2, 0) };
    circle.arcAngles = { kPi, kPi };

    PlanarOptions options;
    options.chordTolerance = 1.0e-20;
    PlanarMap map;
    std::string error;
    EXPECT_FALSE (geo::BuildPlanarMap ({ circle }, options, map, error));
    EXPECT_NE (error.find ("maxSegments"), std::string::npos) << error;
    EXPECT_TRUE (HasDiagnostic (map, PlanarDiagnosticCode::TooManySegments));
    EXPECT_TRUE (map.faces.empty ());
}

TEST (EditPlanarRobustBuild, HugeRadiusTessellationIsRefusedBeforeAllocation)
{
    PlanarCurve circle;
    circle.source = 1;
    circle.closed = true;
    circle.points = { P (0, 0), P (1.0e15, 0) };
    circle.arcAngles = { kPi, kPi };

    PlanarMap map;
    std::string error;
    EXPECT_FALSE (geo::BuildPlanarMap ({ circle }, PlanarOptions {}, map, error));
    EXPECT_NE (error.find ("maxSegments"), std::string::npos) << error;
    EXPECT_TRUE (HasDiagnostic (map, PlanarDiagnosticCode::TooManySegments));
}

TEST (EditPlanarRobustBuild, NormalChordToleranceKeepsCircleArea)
{
    PlanarCurve circle;
    circle.source = 1;
    circle.closed = true;
    circle.points = { P (0, 0), P (2, 0) };
    circle.arcAngles = { kPi, kPi };

    PlanarOptions options;
    options.chordTolerance = 1.0e-5;
    PlanarMap map;
    std::string error;
    ASSERT_TRUE (geo::BuildPlanarMap ({ circle }, options, map, error)) << error;
    ASSERT_EQ (map.faces.size (), 1u);
    EXPECT_NEAR (map.faces.front ().area, kPi, 1.0e-3);
}

TEST (EditPlanarRobustBuild, DuplicateRunUsesTheActualLeavingSegmentOrdinal)
{
    PlanarMap map;
    std::string error;
    ASSERT_TRUE (geo::BuildPlanarMap ({ Closed (1, { P (0, 0), P (0, 0), P (10, 0), P (10, 10), P (0, 10) }) },
                                      PlanarOptions {}, map, error))
        << error;
    ASSERT_EQ (map.faces.size (), 1u);
    EXPECT_EQ (PiecesOf (map.faces.front ()), (std::vector<uint32_t> { 1, 2, 3, 4 }));

    const PlanarEdge* bottom = BottomEdge (map);
    ASSERT_NE (bottom, nullptr);
    ASSERT_EQ (bottom->pieces.size (), 1u);
    EXPECT_EQ (bottom->pieces.front ().piece, 1u);
    EXPECT_DOUBLE_EQ (bottom->pieces.front ().sourceStart, 0.0);
    EXPECT_DOUBLE_EQ (bottom->pieces.front ().sourceEnd, 1.0);
}

TEST (EditPlanarRobustBuild, DuplicateClosingPointUsesOriginalPieces)
{
    PlanarMap map;
    std::string error;
    ASSERT_TRUE (geo::BuildPlanarMap ({ Closed (1, { P (0, 0), P (10, 0), P (10, 10), P (0, 10), P (0, 0) }) },
                                      PlanarOptions {}, map, error))
        << error;
    ASSERT_EQ (map.faces.size (), 1u);
    EXPECT_EQ (PiecesOf (map.faces.front ()), (std::vector<uint32_t> { 0, 1, 2, 3 }));
}

TEST (EditPlanarRobustBuild, ArcLeavingTheActualDuplicateVertexIsAccepted)
{
    PlanarCurve curve = Closed (1, { P (0, 0), P (0, 0), P (10, 0), P (10, 10), P (0, 10) });
    curve.arcAngles = { 0.0, kPi * 0.5, 0.0, 0.0, 0.0 };
    PlanarMap map;
    std::string error;
    ASSERT_TRUE (geo::BuildPlanarMap ({ curve }, PlanarOptions {}, map, error)) << error;
    ASSERT_EQ (map.faces.size (), 1u);
    EXPECT_GT (map.vertices.size (), 4u);
    EXPECT_EQ (CountPiece (map.faces.front (), 1), 1);
}

TEST (EditPlanarRobustBuild, NonzeroArcOnZeroLengthSegmentIsRefused)
{
    PlanarCurve curve = Closed (1, { P (0, 0), P (0, 0), P (10, 0), P (10, 10), P (0, 10) });
    curve.arcAngles = { kPi * 0.5, 0.0, 0.0, 0.0, 0.0 };
    PlanarMap map;
    std::string error;
    EXPECT_FALSE (geo::BuildPlanarMap ({ curve }, PlanarOptions {}, map, error));
    EXPECT_NE (error.find ("zero-length"), std::string::npos) << error;
    EXPECT_TRUE (HasDiagnostic (map, PlanarDiagnosticCode::Degenerate));
}

TEST (EditPlanarRobustBuild, BroadphaseExpandsBoundsByTheNarrowPhaseWeld)
{
    PlanarMap map;
    std::string error;
    ASSERT_TRUE (geo::BuildPlanarMap ({ Open (1, { P (0, 2), P (2, 2) }), Open (2, { P (1, 0), P (1, 1.9999999995) }) },
                                      PlanarOptions {}, map, error))
        << error;
    ASSERT_EQ (map.vertices.size (), 4u);
    ASSERT_EQ (map.edges.size (), 3u);
    bool junction = false;
    for (const auto& vertex : map.vertices) {
        if (std::abs (vertex.x - 1.0) <= 1.0e-9 && std::abs (vertex.y - 2.0) <= 1.0e-9)
            junction = true;
    }
    EXPECT_TRUE (junction);
}

TEST (EditPlanarRobustBuild, DescriptorQuantizationStrictlyRejectsTwoToSixtyThree)
{
    const double limit = std::ldexp (1.0, 63);
    const double side = 1.0e6;
    PlanarOptions options;
    options.snapTolerance = 1.0;
    PlanarMap map;
    std::string error;
    EXPECT_FALSE (geo::BuildPlanarMap (
        { Closed (1, { P (limit, 0), P (limit + side, 0), P (limit + side, side), P (limit, side) }) }, options, map,
        error));
    EXPECT_NE (error.find ("64-bit"), std::string::npos) << error;
    EXPECT_TRUE (HasDiagnostic (map, PlanarDiagnosticCode::Degenerate));
}

TEST (EditPlanarRobustBuild, CollapsedWholeSegmentIsRefusedNotSilentlyOmitted)
{
    PlanarMap map;
    std::string error;
    EXPECT_FALSE (
        geo::BuildPlanarMap ({ Closed (1, { P (0, 0), P (1.0e-10, 0), P (1.0e-10, 1.0e-10), P (0, 1.0e-10) }) },
                             PlanarOptions {}, map, error));
    EXPECT_NE (error.find ("collapse"), std::string::npos) << error;
    EXPECT_TRUE (HasDiagnostic (map, PlanarDiagnosticCode::Degenerate));
    EXPECT_TRUE (map.faces.empty ());
}

TEST (EditPlanarRobustBuild, LooseChordToleranceKeepsClosedCircleTopology)
{
    PlanarCurve circle;
    circle.source = 1;
    circle.closed = true;
    circle.points = { P (0, 0), P (2, 0) };
    circle.arcAngles = { kPi, kPi };

    PlanarOptions options;
    options.chordTolerance = 1.0e6; // Sagitta alone would accept one chord per arc.
    PlanarMap map;
    std::string error;
    ASSERT_TRUE (geo::BuildPlanarMap ({ circle }, options, map, error)) << error;
    ASSERT_EQ (map.faces.size (), 1u);
    EXPECT_NEAR (map.faces.front ().area, 2.0, 1.0e-9);
    EXPECT_GT (map.vertices.size (), 2u);
}

TEST (EditPlanarRobustBuild, NearFullTurnArcIsGovernedByWorkNotAnAngleBand)
{
    PlanarCurve arc;
    arc.source = 1;
    arc.points = { P (0, 0), P (2, 0) };
    arc.arcAngles = { 2.0 * kPi - 1.0e-3, 0.0 };
    PlanarCurve chord;
    chord.source = 2;
    chord.points = { P (2, 0), P (0, 0) };

    PlanarOptions options;
    options.chordTolerance = 1.0e6;
    options.maxSegments = 100;
    PlanarMap map;
    std::string error;
    ASSERT_TRUE (geo::BuildPlanarMap ({ arc, chord }, options, map, error)) << error;
    EXPECT_FALSE (map.faces.empty ());
}

TEST (EditPlanarRobustBuild, CubicArchAreaAndSourceParameters)
{
    PlanarOptions options;
    options.chordTolerance = 1.0e-4;
    PlanarMap map;
    std::string error;
    ASSERT_TRUE (geo::BuildPlanarMap ({ CubicArch () }, options, map, error)) << error;
    ASSERT_EQ (map.faces.size (), 1u);
    EXPECT_NEAR (map.faces.front ().area, 2.4, 1.0e-3);
    EXPECT_EQ (PiecesOf (map.faces.front ()), (std::vector<uint32_t> { 0, 1 }));
    EXPECT_GT (map.vertices.size (), 4u);

    std::vector<std::pair<double, double>> intervals;
    for (const auto& edge : map.edges) {
        for (const auto& piece : edge.pieces) {
            if (piece.source != 1u || piece.piece != 0u)
                continue;
            EXPECT_GE (piece.sourceStart, 0.0);
            EXPECT_LE (piece.sourceEnd, 1.0);
            EXPECT_LT (piece.sourceStart, piece.sourceEnd);
            intervals.push_back ({ piece.sourceStart, piece.sourceEnd });
        }
    }
    ASSERT_FALSE (intervals.empty ());
    std::sort (intervals.begin (), intervals.end ());
    EXPECT_NEAR (intervals.front ().first, 0.0, 1.0e-12);
    EXPECT_NEAR (intervals.back ().second, 1.0, 1.0e-12);
    for (std::size_t i = 1; i < intervals.size (); ++i)
        EXPECT_NEAR (intervals[i].first, intervals[i - 1].second, 1.0e-12);
}

TEST (EditPlanarRobustBuild, EmptyAndZeroHandlesAreEquivalentToStraightEdges)
{
    PlanarCurve empty = Closed (1, { P (0, 0), P (10, 0), P (10, 10), P (0, 10) });
    PlanarCurve zeros = empty;
    zeros.inHandles.assign (4, Vec3 {});
    zeros.outHandles.assign (4, Vec3 {});

    PlanarMap emptyMap;
    PlanarMap zeroMap;
    std::string error;
    ASSERT_TRUE (geo::BuildPlanarMap ({ empty }, PlanarOptions {}, emptyMap, error)) << error;
    ASSERT_TRUE (geo::BuildPlanarMap ({ zeros }, PlanarOptions {}, zeroMap, error)) << error;
    ExpectSameFaces (emptyMap, zeroMap);
}

TEST (EditPlanarRobustBuild, CubicHandleValidationAndArcConflictRefuse)
{
    PlanarCurve square = Closed (1, { P (0, 0), P (10, 0), P (10, 10), P (0, 10) });
    PlanarMap map;
    std::string error;

    PlanarCurve badCount = square;
    badCount.outHandles.resize (3);
    EXPECT_FALSE (geo::BuildPlanarMap ({ badCount }, PlanarOptions {}, map, error));
    EXPECT_NE (error.find ("one handle per vertex"), std::string::npos) << error;

    PlanarCurve nonFinite = square;
    nonFinite.outHandles.assign (4, Vec3 {});
    nonFinite.outHandles[0] = { std::numeric_limits<double>::infinity (), 0.0, 0.0 };
    EXPECT_FALSE (geo::BuildPlanarMap ({ nonFinite }, PlanarOptions {}, map, error));
    EXPECT_NE (error.find ("non-finite outgoing handle"), std::string::npos) << error;

    PlanarCurve conflict;
    conflict.source = 2;
    conflict.points = { P (0, 0), P (2, 0) };
    conflict.arcAngles = { kPi * 0.5, 0.0 };
    conflict.outHandles = { P (0, 1), Vec3 {} };
    EXPECT_FALSE (geo::BuildPlanarMap ({ conflict }, PlanarOptions {}, map, error));
    EXPECT_NE (error.find ("both an arc angle and cubic handles"), std::string::npos) << error;

    PlanarCurve open = Open (3, { P (0, 0), P (2, 0) });
    open.inHandles = { P (0, 1), Vec3 {} };
    EXPECT_FALSE (geo::BuildPlanarMap ({ open }, PlanarOptions {}, map, error));
    EXPECT_NE (error.find ("open start"), std::string::npos) << error;

    open.inHandles.clear ();
    open.outHandles = { Vec3 {}, P (0, 1) };
    EXPECT_FALSE (geo::BuildPlanarMap ({ open }, PlanarOptions {}, map, error));
    EXPECT_NE (error.find ("open end"), std::string::npos) << error;
}

TEST (EditPlanarRobustBuild, CubicTinyToleranceHitsNamedWorkLimit)
{
    PlanarOptions options;
    options.chordTolerance = 1.0e-15;
    options.maxSubdivisionDepth = 4;
    PlanarMap map;
    std::string error;
    EXPECT_FALSE (geo::BuildPlanarMap ({ CubicArch () }, options, map, error));
    EXPECT_NE (error.find ("subdivision depth"), std::string::npos) << error;
    EXPECT_TRUE (HasDiagnostic (map, PlanarDiagnosticCode::WorkLimit));
}

TEST (EditPlanarRobustBuild, CubicDuplicateHandlesUseFirstIncomingAndLastOutgoing)
{
    for (int mode = 0; mode < 2; ++mode) {
        PlanarCurve curve;
        curve.source = 1;
        curve.closed = true;
        curve.points = { P (0, 0), P (2, 0), P (2, 2), P (0, 0), P (0, 0) };
        curve.inHandles.assign (5, Vec3 {});
        curve.outHandles.assign (5, Vec3 {});
        if (mode == 0)
            curve.inHandles[3] = P (0, 2); // Incoming handle belongs to the first duplicate.
        else
            curve.outHandles[0] = P (0, 2); // Outgoing/piece ordinal belongs to the last duplicate.

        PlanarMap map;
        std::string error;
        ASSERT_TRUE (geo::BuildPlanarMap ({ curve }, PlanarOptions {}, map, error)) << error;
        EXPECT_FALSE (map.faces.empty ());
        EXPECT_GT (map.vertices.size (), 4u);
    }

    PlanarCurve bad;
    bad.source = 2;
    bad.closed = true;
    bad.points = { P (0, 0), P (2, 0), P (2, 2), P (0, 0), P (0, 0) };
    bad.outHandles.assign (5, Vec3 {});
    bad.outHandles[3] = P (0, 1); // Leaves a zero-length duplicate segment.
    PlanarMap map;
    std::string error;
    EXPECT_FALSE (geo::BuildPlanarMap ({ bad }, PlanarOptions {}, map, error));
    EXPECT_NE (error.find ("zero-length duplicate"), std::string::npos) << error;
}

TEST (EditPlanarRobustBuild, CubicShuffledSourcesKeepCanonicalFaces)
{
    const std::vector<PlanarCurve> curves { CubicArch (), Closed (2, { P (10, 0), P (12, 0), P (12, 2), P (10, 2) }) };
    PlanarOptions options;
    options.chordTolerance = 1.0e-4;
    PlanarMap expected;
    std::string error;
    ASSERT_TRUE (geo::BuildPlanarMap (curves, options, expected, error)) << error;
    PlanarMap reversed;
    ASSERT_TRUE (geo::BuildPlanarMap ({ curves[1], curves[0] }, options, reversed, error)) << error;
    ExpectSameFaces (expected, reversed);
}
