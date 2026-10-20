#include "Edit/EditPlanar.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace geo = tapioca::geometry;
using geo::FaceCycle;
using geo::PlanarCurve;
using geo::PlanarDiagnosticCode;
using geo::PlanarFace;
using geo::PlanarMap;
using geo::PlanarOptions;
using geo::Vec3;

namespace {

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

const PlanarFace* FaceNear (const PlanarMap& map, double area, double tolerance = 1.0e-7)
{
    for (const auto& face : map.faces)
        if (std::abs (face.area - area) <= tolerance)
            return &face;
    return nullptr;
}

double QuantizedArea (const FaceCycle& cycle)
{
    if (cycle.x.size () < 3)
        return 0.0;
    // Anchor the test helper too: on a georeferenced lattice the absolute
    // products are much larger than the one-cell area being measured.
    const long double originX = static_cast<long double> (cycle.x.front ());
    const long double originY = static_cast<long double> (cycle.y.front ());
    long double twice = 0.0L;
    for (std::size_t i = 0; i < cycle.x.size (); ++i) {
        const std::size_t j = (i + 1) % cycle.x.size ();
        const long double ax = static_cast<long double> (cycle.x[i]) - originX;
        const long double ay = static_cast<long double> (cycle.y[i]) - originY;
        const long double bx = static_cast<long double> (cycle.x[j]) - originX;
        const long double by = static_cast<long double> (cycle.y[j]) - originY;
        twice += ax * by - bx * ay;
    }
    return static_cast<double> (twice * 0.5L);
}

bool StrictlyInsideOpenSquare (Vec3 point, double low, double high)
{
    return point.x > low && point.x < high && point.y > low && point.y < high;
}

void ExpectBuild (const std::vector<PlanarCurve>& curves, const PlanarOptions& options, PlanarMap& map)
{
    std::string error;
    ASSERT_TRUE (geo::BuildPlanarMap (curves, options, map, error)) << error;
}

} // namespace

TEST (EditPlanarRobustFaces, NarrowAnnulusKeepsInnerHoleAndStrictSeed)
{
    for (double inset : { 1.0e-8, 2.0e-7 }) {
        SCOPED_TRACE (inset);
        const double far = 10.0 - inset;
        const double innerWidth = far - inset;
        const double innerArea = innerWidth * innerWidth;
        const double annulusArea = 100.0 - innerArea;

        PlanarOptions options;
        options.snapTolerance = 1.0e-10;
        options.minArea = 0.0;
        PlanarMap map;
        ExpectBuild ({ Closed (1, { P (0, 0), P (10, 0), P (10, 10), P (0, 10) }),
                       Closed (2, { P (inset, inset), P (far, inset), P (far, far), P (inset, far) }) },
                     options, map);

        ASSERT_EQ (map.faces.size (), 2u);
        const PlanarFace* annulus = FaceNear (map, annulusArea);
        const PlanarFace* island = FaceNear (map, innerArea);
        ASSERT_NE (annulus, nullptr);
        ASSERT_NE (island, nullptr);
        EXPECT_NE (annulus, island);

        ASSERT_EQ (annulus->holes.size (), 1u);
        ASSERT_EQ (annulus->descriptor.holes.size (), 1u);
        EXPECT_TRUE (annulus->descriptor.holes.front ().hole);
        EXPECT_LT (QuantizedArea (annulus->descriptor.holes.front ()), 0.0);
        EXPECT_TRUE (map.cycles[annulus->holes.front ()].hole);
        EXPECT_NEAR (map.cycles[annulus->holes.front ()].area, -innerArea, 1.0e-9);

        EXPECT_NEAR (annulus->area, annulusArea, 1.0e-9);
        EXPECT_NEAR (island->area, innerArea, 1.0e-9);
        EXPECT_NEAR (annulus->area + island->area, 100.0, 1.0e-9);
        EXPECT_EQ (annulus->sources, (std::vector<geo::Id> { 1, 2 }));
        EXPECT_EQ (island->sources, (std::vector<geo::Id> { 2 }));
        EXPECT_TRUE (island->holes.empty ());

        // The seed must not be the old boundary fallback and must not land in
        // the hole: the ring is only one inset wide.
        EXPECT_TRUE (StrictlyInsideOpenSquare (annulus->seed, 0.0, 10.0));
        EXPECT_FALSE (StrictlyInsideOpenSquare (annulus->seed, inset, far));
        EXPECT_TRUE (StrictlyInsideOpenSquare (island->seed, inset, far));
    }
}

TEST (EditPlanarRobustFaces, GeoreferencedSquareAreaIsTranslationStable)
{
    for (double origin : { 1.0e8, 1.0e9 }) {
        SCOPED_TRACE (origin);
        PlanarMap map;
        ExpectBuild ({ Closed (1, { P (origin, origin), P (origin + 1.0, origin), P (origin + 1.0, origin + 1.0),
                                    P (origin, origin + 1.0) }) },
                     PlanarOptions {}, map);
        ASSERT_EQ (map.faces.size (), 1u);
        EXPECT_TRUE (std::isfinite (map.faces.front ().area));
        EXPECT_NEAR (map.faces.front ().area, 1.0, 1.0e-12);
        EXPECT_TRUE (StrictlyInsideOpenSquare (map.faces.front ().seed, origin, origin + 1.0));
        EXPECT_GT (QuantizedArea (map.faces.front ().descriptor.outer), 0.0);
    }
}

TEST (EditPlanarRobustFaces, UnrepresentableAreaRefusesInsteadOfPublishingInfinity)
{
    PlanarOptions options;
    options.snapTolerance = 1.0e190;
    options.minArea = 0.0;
    const double side = 1.0e200;
    PlanarMap map;
    std::string error;
    EXPECT_FALSE (geo::BuildPlanarMap ({ Closed (1, { P (0, 0), P (side, 0), P (side, side), P (0, side) }) }, options,
                                       map, error));
    EXPECT_FALSE (error.empty ());
    // The intersection gate now refuses this span before face-area products.
    EXPECT_NE (error.find ("finite planar intersection arithmetic"), std::string::npos) << error;
    EXPECT_TRUE (HasDiagnostic (map, PlanarDiagnosticCode::Degenerate));
    EXPECT_TRUE (map.faces.empty ());

    // Retain independent coverage of the face builder's own area guard: supply
    // an already valid square topology whose coordinates are scaled afterwards.
    PlanarMap areaMap;
    ExpectBuild ({ Closed (1, { P (0, 0), P (1, 0), P (1, 1), P (0, 1) }) }, PlanarOptions {}, areaMap);
    for (auto& vertex : areaMap.vertices) {
        vertex.x *= side;
        vertex.y *= side;
    }
    areaMap.cycles.clear ();
    areaMap.faces.clear ();
    areaMap.diagnostics.clear ();
    EXPECT_FALSE (geo::detail::BuildFaces (options, areaMap, error));
    EXPECT_NE (error.find ("area"), std::string::npos) << error;
    EXPECT_TRUE (HasDiagnostic (areaMap, PlanarDiagnosticCode::Degenerate));
    EXPECT_TRUE (areaMap.faces.empty ());
}

TEST (EditPlanarRobustFaces, NonSeparatingBridgeIsNotAFaceBoundaryContributor)
{
    const PlanarCurve outer = Closed (1, { P (0, 0), P (10, 0), P (10, 10), P (0, 10) });
    const PlanarCurve inner = Closed (2, { P (3, 3), P (7, 3), P (7, 7), P (3, 7) });
    const PlanarCurve guide = Open (3, { P (0, 0), P (3, 3) });

    PlanarMap map;
    ExpectBuild ({ outer, inner, guide }, PlanarOptions {}, map);
    ASSERT_EQ (map.faces.size (), 2u);
    const PlanarFace* annulus = FaceNear (map, 84.0);
    const PlanarFace* island = FaceNear (map, 16.0);
    ASSERT_NE (annulus, nullptr);
    ASSERT_NE (island, nullptr);
    EXPECT_NE (annulus, island);

    EXPECT_NEAR (annulus->area, 84.0, 1.0e-12);
    EXPECT_NEAR (island->area, 16.0, 1.0e-12);
    EXPECT_NEAR (annulus->area + island->area, 100.0, 1.0e-12);
    EXPECT_EQ (annulus->sources, (std::vector<geo::Id> { 1, 2 }));
    EXPECT_EQ (island->sources, (std::vector<geo::Id> { 2 }));
    EXPECT_TRUE (island->holes.empty ());

    ASSERT_EQ (annulus->holes.size (), 1u);
    ASSERT_EQ (annulus->descriptor.holes.size (), 1u);
    EXPECT_TRUE (annulus->descriptor.holes.front ().hole);
    EXPECT_LT (QuantizedArea (annulus->descriptor.holes.front ()), 0.0);
    EXPECT_NEAR (map.cycles[annulus->holes.front ()].area, -16.0, 1.0e-12);
    EXPECT_TRUE (map.cycles[annulus->holes.front ()].hole);

    // Clean outer and hole cycles: four points each, no repeated half-edge.
    EXPECT_EQ (annulus->descriptor.outer.x.size (), 4u);
    EXPECT_EQ (annulus->descriptor.holes.front ().x.size (), 4u);

    std::size_t guideEdge = map.edges.size ();
    for (std::size_t i = 0; i < map.edges.size (); ++i) {
        const auto& sources = map.edges[i].sources;
        if (std::find (sources.begin (), sources.end (), geo::Id { 3 }) != sources.end ()) {
            EXPECT_EQ (guideEdge, map.edges.size ()) << "the guide must not be duplicated";
            guideEdge = i;
        }
    }
    ASSERT_LT (guideEdge, map.edges.size ());
    std::size_t guideHalfEdges = 0;
    for (const auto& halfEdge : map.halfEdges)
        if (halfEdge.edge == guideEdge)
            ++guideHalfEdges;
    EXPECT_EQ (guideHalfEdges, 2u);

    const auto checkBoundaryCycle = [&] (const std::vector<uint32_t>& cycle) {
        std::set<uint32_t> seen;
        for (std::size_t i = 0; i < cycle.size (); ++i) {
            EXPECT_TRUE (seen.insert (cycle[i]).second) << "a boundary half-edge was repeated";
            const geo::PlanarHalfEdge& halfEdge = map.halfEdges[cycle[i]];
            EXPECT_EQ (halfEdge.next, cycle[(i + 1) % cycle.size ()]);
            EXPECT_NE (halfEdge.edge, guideEdge) << "the bridge leaked into a face boundary";
            for (geo::Id source : map.edges[halfEdge.edge].sources)
                EXPECT_NE (source, geo::Id { 3 });
        }
    };
    checkBoundaryCycle (annulus->halfEdges);
    checkBoundaryCycle (map.cycles[annulus->holes.front ()].halfEdges);

    for (const auto& piece : annulus->descriptor.sourcePieces)
        EXPECT_NE (piece.source, geo::Id { 3 });
    EXPECT_TRUE (StrictlyInsideOpenSquare (annulus->seed, 0.0, 10.0));
    EXPECT_FALSE (StrictlyInsideOpenSquare (annulus->seed, 3.0, 7.0));

    // The reduced walk still has one incoming next per half-edge, including the
    // retained bridge pair.
    std::vector<uint32_t> incoming (map.halfEdges.size (), 0);
    for (const auto& halfEdge : map.halfEdges)
        ++incoming[halfEdge.next];
    for (uint32_t count : incoming)
        EXPECT_EQ (count, 1u);

    PlanarMap reordered;
    ExpectBuild ({ inner, guide, outer }, PlanarOptions {}, reordered);
    ASSERT_EQ (reordered.faces.size (), map.faces.size ());
    for (std::size_t i = 0; i < map.faces.size (); ++i) {
        EXPECT_EQ (map.faces[i].descriptor, reordered.faces[i].descriptor);
        EXPECT_EQ (map.faces[i].sources, reordered.faces[i].sources);
        EXPECT_NEAR (map.faces[i].area, reordered.faces[i].area, 1.0e-12);
    }
}
