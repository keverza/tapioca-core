#include "Edit/EditPlanar.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace geo = tapioca::geometry;
using geo::FaceCycle;
using geo::FaceDescriptor;
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

bool HasDiagnostic (const PlanarMap& map, PlanarDiagnosticCode code)
{
    return std::any_of (map.diagnostics.begin (), map.diagnostics.end (),
                        [&] (const geo::PlanarDiagnostic& diagnostic) { return diagnostic.code == code; });
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

} // namespace

TEST (EditPlanarIdentity, ShuffledInputKeepsFaceKeysAndLocators)
{
    const std::vector<PlanarCurve> curves { Closed (100, { P (0, 0), P (10, 0), P (10, 10), P (0, 10) }),
                                            Closed (200, { P (2, 2), P (4, 2), P (4, 4), P (2, 4) }),
                                            Closed (300, { P (20, 0), P (25, 0), P (25, 5), P (20, 5) }),
                                            Closed (400, { P (4, 4), P (6, 4), P (6, 6), P (4, 6) }) };
    PlanarMap expected;
    std::string error;
    ASSERT_TRUE (geo::BuildPlanarMap (curves, PlanarOptions {}, expected, error)) << error;
    ASSERT_GE (expected.faces.size (), 3u);

    std::mt19937 generator (1234);
    for (int iteration = 0; iteration < 8; ++iteration) {
        std::vector<PlanarCurve> shuffled = curves;
        std::shuffle (shuffled.begin (), shuffled.end (), generator);
        PlanarMap actual;
        ASSERT_TRUE (geo::BuildPlanarMap (shuffled, PlanarOptions {}, actual, error)) << error;
        ExpectSameFaces (expected, actual);
        ASSERT_EQ (expected.diagnostics.size (), actual.diagnostics.size ());
        for (std::size_t i = 0; i < expected.diagnostics.size (); ++i) {
            EXPECT_EQ (expected.diagnostics[i].code, actual.diagnostics[i].code);
            EXPECT_EQ (expected.diagnostics[i].sourceA, actual.diagnostics[i].sourceA);
            EXPECT_EQ (expected.diagnostics[i].sourceB, actual.diagnostics[i].sourceB);
            EXPECT_EQ (expected.diagnostics[i].message, actual.diagnostics[i].message);
        }
    }
}

TEST (EditPlanarIdentity, ShuffledGapHealingKeepsCanonicalFaces)
{
    const std::vector<PlanarCurve> curves { Open (1, { P (0, 0), P (10, 0) }), Open (2, { P (10, 0), P (10, 10) }),
                                            Open (3, { P (10, 10), P (0, 10) }),
                                            Open (4, { P (0, 10), P (0, 0.002) }) };
    PlanarOptions options;
    options.gapTolerance = 0.003;
    PlanarMap expected;
    std::string error;
    ASSERT_TRUE (geo::BuildPlanarMap (curves, options, expected, error)) << error;
    ASSERT_EQ (expected.faces.size (), 1u);
    EXPECT_NEAR (expected.faces.front ().area, 100.0, 1.0e-9);

    std::mt19937 generator (99);
    for (int iteration = 0; iteration < 6; ++iteration) {
        std::vector<PlanarCurve> shuffled = curves;
        std::shuffle (shuffled.begin (), shuffled.end (), generator);
        PlanarMap actual;
        ASSERT_TRUE (geo::BuildPlanarMap (shuffled, options, actual, error)) << error;
        ExpectSameFaces (expected, actual);
    }
}

TEST (EditPlanarIdentity, FaceLocatorsAreRevisionLocalRanksNotAuthoredIds)
{
    PlanarOptions options;
    PlanarMap first;
    std::string error;
    ASSERT_TRUE (geo::BuildPlanarMap ({ Closed (100, { P (0, 0), P (10, 0), P (10, 10), P (0, 10) }),
                                        Closed (200, { P (2, 2), P (4, 2), P (4, 4), P (2, 4) }) },
                                      options, first, error))
        << error;
    ASSERT_EQ (first.faces.size (), 2u);
    for (std::size_t i = 0; i < first.faces.size (); ++i) {
        EXPECT_EQ (first.faces[i].id, i + 1);
        EXPECT_NE (first.faces[i].id, 100u);
        EXPECT_NE (first.faces[i].id, 200u);
    }
    const FaceDescriptor firstOuter = first.faces.front ().descriptor;

    PlanarMap second;
    ASSERT_TRUE (geo::BuildPlanarMap ({ Closed (100, { P (0, 0), P (4, 0), P (4, 4), P (0, 4) }),
                                        Closed (200, { P (10, 10), P (15, 10), P (15, 15), P (10, 15) }) },
                                      options, second, error))
        << error;
    ASSERT_EQ (second.faces.size (), 2u);
    EXPECT_EQ (second.faces[0].id, 1u);
    EXPECT_EQ (second.faces[1].id, 2u);
    EXPECT_NE (firstOuter, second.faces[0].descriptor);
}

TEST (EditPlanarIdentity, FullDescriptorComparisonIgnoresNumericId)
{
    PlanarFace a;
    PlanarFace b;
    a.descriptor.sourcePieces = { { 7, 0 }, { 8, 1 } };
    b.descriptor.sourcePieces = a.descriptor.sourcePieces;
    a.descriptor.outer.x = { 0, 10, 10, 0 };
    a.descriptor.outer.y = { 0, 0, 10, 10 };
    b.descriptor.outer = a.descriptor.outer;
    a.id = 1;
    b.id = 99;
    EXPECT_EQ (a.descriptor, b.descriptor);

    PlanarMap map;
    map.faces = { a, b };
    map.faces[0].seed = P (3, 3);
    map.faces[1].seed = P (7, 7);
    std::string error;
    EXPECT_FALSE (geo::detail::AssignFaceLocators (map, error));
    EXPECT_NE (error.find ("FaceKeyAmbiguous"), std::string::npos) << error;
    EXPECT_TRUE (HasDiagnostic (map, PlanarDiagnosticCode::FaceKeyAmbiguous));
    EXPECT_GE (map.faces.size (), 2u);
    EXPECT_EQ (map.faces[0].descriptor, map.faces[1].descriptor);

    std::swap (map.faces[0], map.faces[1]);
    EXPECT_FALSE (geo::detail::AssignFaceLocators (map, error));
    EXPECT_NE (error.find ("FaceKeyAmbiguous"), std::string::npos) << error;
}

TEST (EditPlanarIdentity, AssignFaceLocatorsOrdersDistinctDescriptors)
{
    PlanarMap map;
    PlanarFace high;
    PlanarFace low;
    high.descriptor.sourcePieces = { { 9, 0 } };
    low.descriptor.sourcePieces = { { 2, 0 } };
    map.faces = { high, low };
    std::string error;
    ASSERT_TRUE (geo::detail::AssignFaceLocators (map, error)) << error;
    ASSERT_EQ (map.faces.size (), 2u);
    EXPECT_EQ (map.faces[0].descriptor.sourcePieces.front ().source, 2u);
    EXPECT_EQ (map.faces[1].descriptor.sourcePieces.front ().source, 9u);
    EXPECT_EQ (map.faces[0].id, 1u);
    EXPECT_EQ (map.faces[1].id, 2u);
}

TEST (EditPlanarIdentity, OverlappingSymmetricCirclesCollideOnCoarseDescriptorGrid)
{
    PlanarCurve left;
    left.source = 1;
    left.closed = true;
    left.points = { P (-3, 0), P (1, 0) };
    left.arcAngles = { kPi, kPi };
    PlanarCurve right;
    right.source = 2;
    right.closed = true;
    right.points = { P (-1, 0), P (3, 0) };
    right.arcAngles = { kPi, kPi };

    PlanarOptions options;
    options.snapTolerance = 10.0; // Coarse descriptor lattice only.
    options.chordTolerance = 1.0e-3;
    options.minArea = 1.0e-9;

    PlanarMap map;
    std::string error;
    EXPECT_FALSE (geo::BuildPlanarMap ({ left, right }, options, map, error));
    EXPECT_NE (error.find ("FaceKeyAmbiguous"), std::string::npos) << error;
    EXPECT_TRUE (HasDiagnostic (map, PlanarDiagnosticCode::FaceKeyAmbiguous));
    ASSERT_EQ (map.faces.size (), 3u); // Lens plus two crescents survived.
    for (const auto& face : map.faces)
        EXPECT_EQ (face.descriptor, map.faces.front ().descriptor);
    EXPECT_GT (map.edges.size (), 4u);

    // The three arrangement faces survived as geometry; the refusal is the
    // descriptor collision, not a silent merge or cull.
    double area = 0.0;
    std::set<std::pair<long long, long long>> seeds;
    for (const auto& face : map.faces) {
        area += face.area;
        seeds.insert ({ static_cast<long long> (std::llround (face.seed.x * 1000.0)),
                        static_cast<long long> (std::llround (face.seed.y * 1000.0)) });
        EXPECT_EQ (face.sources, (std::vector<geo::Id> { 1, 2 }));
        EXPECT_EQ (face.descriptor.sourcePieces.size (), 4u);
        for (const auto& piece : face.descriptor.sourcePieces)
            EXPECT_LT (piece.piece, 2u); // Original semicircle segment, not a chord index.
    }
    EXPECT_GE (seeds.size (), 2u);
    // Two radius-2 circles with centre distance 2: union area minus the lens.
    const double overlap = 8.0 * std::acos (0.5) - 2.0 * std::sqrt (3.0);
    EXPECT_NEAR (area, 8.0 * kPi - overlap, 0.05);

    PlanarMap reversed;
    EXPECT_FALSE (geo::BuildPlanarMap ({ right, left }, options, reversed, error));
    EXPECT_TRUE (HasDiagnostic (reversed, PlanarDiagnosticCode::FaceKeyAmbiguous));
    ASSERT_EQ (reversed.faces.size (), 3u);
    for (const auto& face : reversed.faces)
        EXPECT_EQ (face.descriptor, reversed.faces.front ().descriptor);
}

TEST (EditPlanarIdentity, QuantizationRoundsHalfAwayFromZero)
{
    PlanarOptions options;
    options.snapTolerance = 1.0;
    options.minArea = 1.0e-12;
    PlanarMap map;
    std::string error;
    ASSERT_TRUE (geo::BuildPlanarMap ({ Closed (1, { P (-0.5, -0.5), P (0.5, -0.5), P (0.5, 0.5), P (-0.5, 0.5) }) },
                                      options, map, error))
        << error;
    ASSERT_EQ (map.faces.size (), 1u);
    const FaceCycle& outer = map.faces.front ().descriptor.outer;
    std::set<std::pair<long long, long long>> points;
    for (std::size_t i = 0; i < outer.x.size (); ++i)
        points.insert ({ static_cast<long long> (outer.x[i]), static_cast<long long> (outer.y[i]) });
    EXPECT_TRUE (points.count ({ -1, -1 }) == 1);
    EXPECT_TRUE (points.count ({ 1, -1 }) == 1);
    EXPECT_TRUE (points.count ({ 1, 1 }) == 1);
    EXPECT_TRUE (points.count ({ -1, 1 }) == 1);
}

TEST (EditPlanarIdentity, CoarseDescriptorGridDoesNotMergeDistinctFaces)
{
    PlanarOptions options;
    options.snapTolerance = 1000.0;
    options.gapTolerance = 0.0;
    PlanarMap map;
    std::string error;
    ASSERT_TRUE (geo::BuildPlanarMap ({ Closed (1, { P (0, 0), P (10, 0), P (10, 10), P (0, 10) }),
                                        Closed (2, { P (20, 0), P (30, 0), P (30, 10), P (20, 10) }) },
                                      options, map, error))
        << error;
    ASSERT_EQ (map.faces.size (), 2u);
    EXPECT_NEAR (map.faces[0].area + map.faces[1].area, 200.0, 1.0e-9);
    EXPECT_NE (map.faces[0].descriptor, map.faces[1].descriptor);
}

TEST (EditPlanarIdentity, GapZeroKeepsDistinctFacesSeparate)
{
    PlanarOptions options;
    options.snapTolerance = 1000.0; // Coarse key lattice must not heal geometry.
    options.gapTolerance = 0.0;
    PlanarMap map;
    std::string error;
    ASSERT_TRUE (geo::BuildPlanarMap ({ Closed (1, { P (0, 0), P (10, 0), P (10, 10), P (0, 10) }),
                                        Closed (2, { P (10.002, 0), P (20.002, 0), P (20.002, 10), P (10.002, 10) }) },
                                      options, map, error))
        << error;
    ASSERT_EQ (map.faces.size (), 2u);
    EXPECT_NEAR (map.faces[0].area, 100.0, 1.0e-9);
    EXPECT_NEAR (map.faces[1].area, 100.0, 1.0e-9);
    EXPECT_FALSE (HasDiagnostic (map, PlanarDiagnosticCode::OpenGap));
}

TEST (EditPlanarIdentity, RoundoffWeldJoinsEndpointsWithACoarseDescriptorLattice)
{
    const double roundoff = 1.0e-12;
    PlanarCurve a = Closed (1, { P (0, 0), P (10, 0), P (10, 10), P (0, 10) });
    PlanarCurve b = Closed (2, { P (0, 10), P (10, 10), P (10, 0), P (0, 0) });
    for (auto& point : b.points) {
        point.x += roundoff;
        point.y -= roundoff;
    }
    PlanarOptions options;
    options.snapTolerance = 1000.0;
    PlanarMap map;
    std::string error;
    ASSERT_TRUE (geo::BuildPlanarMap ({ a, b }, options, map, error)) << error;
    ASSERT_EQ (map.faces.size (), 1u);
    EXPECT_NEAR (map.faces.front ().area, 100.0, 1.0e-9);
    EXPECT_EQ (map.faces.front ().sources, (std::vector<geo::Id> { 1, 2 }));
}

TEST (EditPlanarIdentity, DuplicateSourceIdsAreRefused)
{
    PlanarMap map;
    std::string error;
    EXPECT_FALSE (geo::BuildPlanarMap (
        { Closed (5, { P (0, 0), P (1, 0), P (1, 1) }), Closed (5, { P (2, 2), P (3, 2), P (3, 3) }) },
        PlanarOptions {}, map, error));
    EXPECT_NE (error.find ("duplicate source ID"), std::string::npos) << error;
    EXPECT_TRUE (map.faces.empty ());
}

TEST (EditPlanarIdentity, MalformedCurvesAreRefused)
{
    PlanarMap map;
    std::string error;
    EXPECT_FALSE (geo::BuildPlanarMap ({ Open (1, { P (0, 0) }) }, PlanarOptions {}, map, error));
    EXPECT_FALSE (error.empty ());

    PlanarCurve duplicate;
    duplicate.source = 2;
    duplicate.closed = true;
    duplicate.points = { P (0, 0), P (0, 0) };
    EXPECT_FALSE (geo::BuildPlanarMap ({ duplicate }, PlanarOptions {}, map, error));
    EXPECT_FALSE (error.empty ());

    PlanarCurve nonFinite;
    nonFinite.source = 3;
    nonFinite.points = { P (0, 0), P (std::numeric_limits<double>::infinity (), 1) };
    EXPECT_FALSE (geo::BuildPlanarMap ({ nonFinite }, PlanarOptions {}, map, error));
    EXPECT_FALSE (error.empty ());

    PlanarCurve badAngles;
    badAngles.source = 4;
    badAngles.points = { P (0, 0), P (1, 0), P (1, 1) };
    badAngles.arcAngles = { 0.0, 0.0 };
    EXPECT_FALSE (geo::BuildPlanarMap ({ badAngles }, PlanarOptions {}, map, error));
    EXPECT_FALSE (error.empty ());

    PlanarCurve unusedArc;
    unusedArc.source = 5;
    unusedArc.points = { P (0, 0), P (1, 0) };
    unusedArc.arcAngles = { 0.0, 0.5 };
    EXPECT_FALSE (geo::BuildPlanarMap ({ unusedArc }, PlanarOptions {}, map, error));
    EXPECT_NE (error.find ("no leaving segment"), std::string::npos) << error;
}

TEST (EditPlanarIdentity, InvalidOptionsAreRefused)
{
    const std::vector<PlanarCurve> curves { Closed (1, { P (0, 0), P (1, 0), P (1, 1) }) };
    PlanarMap map;
    std::string error;
    for (double snap : { 0.0, -1.0, std::numeric_limits<double>::infinity () }) {
        PlanarOptions options;
        options.snapTolerance = snap;
        EXPECT_FALSE (geo::BuildPlanarMap (curves, options, map, error));
        EXPECT_FALSE (error.empty ());
    }
    PlanarOptions negativeGap;
    negativeGap.gapTolerance = -0.1;
    EXPECT_FALSE (geo::BuildPlanarMap (curves, negativeGap, map, error));
    PlanarOptions zeroChord;
    zeroChord.chordTolerance = 0.0;
    EXPECT_FALSE (geo::BuildPlanarMap (curves, zeroChord, map, error));
    PlanarOptions zeroMax;
    zeroMax.maxSegments = 0;
    EXPECT_FALSE (geo::BuildPlanarMap (curves, zeroMax, map, error));
}

TEST (EditPlanarIdentity, TooManySegmentsRefusesWithoutTruncation)
{
    PlanarOptions options;
    options.maxSegments = 3;
    PlanarMap map;
    std::string error;
    EXPECT_FALSE (
        geo::BuildPlanarMap ({ Closed (1, { P (0, 0), P (10, 0), P (10, 10), P (0, 10) }) }, options, map, error));
    EXPECT_NE (error.find ("maxSegments"), std::string::npos) << error;
    EXPECT_TRUE (HasDiagnostic (map, PlanarDiagnosticCode::TooManySegments));
    EXPECT_TRUE (map.faces.empty ());
}

TEST (EditPlanarIdentity, CoordinateAndDescriptorOverflowRefuse)
{
    {
        PlanarOptions options;
        options.snapTolerance = 1.0e-3;
        PlanarMap map;
        std::string error;
        EXPECT_FALSE (geo::BuildPlanarMap ({ Closed (1, { P (1.0e19, 1.0e19), P (1.0e19 + 1, 1.0e19),
                                                          P (1.0e19 + 1, 1.0e19 + 1), P (1.0e19, 1.0e19 + 1) }) },
                                           options, map, error));
        EXPECT_FALSE (error.empty ());
        EXPECT_TRUE (HasDiagnostic (map, PlanarDiagnosticCode::Degenerate));
    }
    {
        PlanarOptions options;
        options.snapTolerance = 1.0e-3;
        PlanarMap map;
        std::string error;
        EXPECT_FALSE (
            geo::BuildPlanarMap ({ Closed (1, { P (1.0e19, 1.0e19), P (1.0e19 + 3.0e6, 1.0e19),
                                                P (1.0e19 + 3.0e6, 1.0e19 + 3.0e6), P (1.0e19, 1.0e19 + 3.0e6) }) },
                                 options, map, error));
        EXPECT_NE (error.find ("64-bit"), std::string::npos) << error;
    }
}

TEST (EditPlanarIdentity, InputCurvesAreNotMutated)
{
    PlanarCurve arc;
    arc.source = 1;
    arc.closed = true;
    arc.points = { P (0, 0), P (2, 0) };
    arc.arcAngles = { kPi, kPi };
    PlanarCurve ring = Closed (2, { P (10, 0), P (20, 0), P (20, 10), P (10, 10) });
    const std::vector<Vec3> arcPoints = arc.points;
    const std::vector<double> arcAngles = arc.arcAngles;
    const std::vector<Vec3> ringPoints = ring.points;
    PlanarMap map;
    std::string error;
    EXPECT_TRUE (geo::BuildPlanarMap ({ arc, ring }, PlanarOptions {}, map, error)) << error;
    EXPECT_EQ (arc.points, arcPoints);
    EXPECT_EQ (arc.arcAngles, arcAngles);
    EXPECT_EQ (ring.points, ringPoints);
    EXPECT_TRUE (arc.closed);
    EXPECT_EQ (arc.source, 1u);
    EXPECT_EQ (ring.source, 2u);
}
