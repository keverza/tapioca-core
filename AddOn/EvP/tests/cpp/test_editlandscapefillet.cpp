// Live corner radii on a landscape source: the rounded outline is one ring in drawing order,
// every chord of a corner's arc carries both incident authored segments (never a sample
// index), and the region's semantic identity does not depend on how finely the arc is cut.
#include "Edit/EditDocument.hpp"
#include "Edit/EditLandscape.hpp"
#include "Edit/EditSession.hpp"
#include "Edit/EditShape.hpp"
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>

namespace edit = geomsrv::edit;
namespace geo = tapioca::geometry;

namespace {
constexpr double kPi = 3.14159265358979323846;

struct Rounded {
    edit::Document doc;
    edit::Id source = 0, entity = 0, op = 0;
};
// A 10 m square at the origin, its corners rounded to `radius` (0 keeps them sharp).
Rounded Square (double radius, bool closed = true)
{
    Rounded r;
    std::string error;
    EXPECT_TRUE (geo::StartLandscape (r.doc, { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, error))
        << error;
    r.entity = edit::AddPolyline (r.doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } }, closed);
    if (radius > 0) {
        std::vector<edit::Id> corners;
        const auto& vertices = edit::Find (r.doc, r.entity)->vertices;
        for (size_t i = closed ? 0 : 1; i < (closed ? vertices.size () : vertices.size () - 1); ++i)
            corners.push_back (vertices[i].id); // an open guide's ends are not corners
        EXPECT_TRUE (edit::SetCornerRadius (r.doc, r.entity, corners, radius, error)) << error;
    }
    geo::LandscapeSource source;
    source.entityId = r.entity;
    EXPECT_TRUE (geo::AddLandscapeSource (r.doc, source, r.source, error)) << error;
    geo::LandscapeOperation op;
    op.kind = closed ? geo::OperationKind::RegionSelect : geo::OperationKind::Stroke;
    op.inputs = { r.source };
    EXPECT_TRUE (geo::AddLandscapeOperation (r.doc, op, r.op, error)) << error;
    geo::HardscapeObject object;
    object.footprintOperation = r.op;
    edit::Id id = 0;
    EXPECT_TRUE (geo::AddHardscapeObject (r.doc, object, id, error)) << error;
    return r;
}
geo::LandscapeResult Evaluate (const edit::Document& doc)
{
    geo::LandscapeResult result;
    EXPECT_TRUE (geo::EvaluateLandscape (doc, result)) << result.error;
    return result;
}
std::vector<edit::Id> SegmentIds (const edit::Document& doc, edit::Id entity)
{
    std::vector<edit::Id> ids;
    for (const auto& s : edit::Find (doc, entity)->segments)
        ids.push_back (s.id);
    return ids;
}
// Which corner of the square a point is rounded off, or -1 on a straight leg.
int CornerZone (edit::Vec3 p, double radius)
{
    const double lo = radius - 1e-9, hi = 10 - radius + 1e-9;
    const bool inX = p.x > lo && p.x<hi, inY = p.y> lo && p.y < hi;
    if (inX || inY)
        return -1;
    return p.x < 5 ? (p.y < 5 ? 0 : 3) : (p.y < 5 ? 1 : 2);
}
} // namespace

TEST (EditLandscapeFillet, ARoundedSourceIsOneRingWithTheAnalyticArea)
{
    for (const double radius : { 0.5, 2.0, 4.0 }) {
        auto r = Square (radius);
        const auto result = Evaluate (r.doc);
        ASSERT_EQ (result.regions.size (), 1u) << radius;
        ASSERT_EQ (result.regions[0].rings.size (), 1u) << radius;
        const double analytic = 100 - (4 - kPi) * radius * radius;
        EXPECT_NEAR (result.regions[0].area, analytic, 0.02 * radius * radius) << radius;
        EXPECT_LT (result.regions[0].area, analytic + 1e-9) << "chords lie inside the arc";
    }
}

TEST (EditLandscapeFillet, EveryArcChordCarriesBothIncidentSegments)
{
    const double radius = 2.0;
    auto r = Square (radius);
    const auto segments = SegmentIds (r.doc, r.entity); // segment i runs from vertex i to i + 1
    const auto result = Evaluate (r.doc);
    ASSERT_EQ (result.regions.size (), 1u);
    size_t arcPieces = 0;
    for (const auto& piece : result.regions[0].boundary[0]) {
        const int corner = CornerZone ((piece.a + piece.b) * 0.5, radius);
        std::vector<edit::Id> got;
        for (const auto& ref : piece.support) {
            EXPECT_EQ (ref.source, r.source);
            got.push_back (ref.segment);
        }
        std::sort (got.begin (), got.end ());
        if (corner < 0) {
            EXPECT_EQ (got.size (), 1u) << "a straight leg is one segment's";
            continue;
        }
        ++arcPieces;
        std::vector<edit::Id> incident { segments[(corner + 3) % 4], segments[size_t (corner)] };
        std::sort (incident.begin (), incident.end ());
        EXPECT_EQ (got, incident) << "corner " << corner;
    }
    EXPECT_GE (arcPieces, 8u) << "four arcs, each cut into chords";
}

TEST (EditLandscapeFillet, ArcTessellationAndRadiusDoNotChangeTheRegionDescriptor)
{
    const auto sharp = Evaluate (Square (0).doc).regions.at (0).descriptor.boundaries;
    for (const double radius : { 0.5, 2.0, 4.0 }) {
        // Sources are allocated in the same order, so the IDs match across documents.
        EXPECT_EQ (Evaluate (Square (radius).doc).regions.at (0).descriptor.boundaries, sharp) << radius;
    }
    // Editing the radius in place keeps the persistent region.
    auto r = Square (2.0);
    const auto before = Evaluate (r.doc).regions.at (0).regionId;
    std::vector<edit::Id> corners;
    for (const auto& v : edit::Find (r.doc, r.entity)->vertices)
        corners.push_back (v.id);
    edit::Session session (r.doc);
    ASSERT_TRUE (session.Apply ([&] (edit::Document& d, std::string& e) {
        return edit::SetCornerRadius (d, r.entity, corners, 3.5, e);
    })) << session.Message ();
    const auto after = Evaluate (session.Doc ());
    ASSERT_EQ (after.regions.size (), 1u);
    EXPECT_EQ (after.regions[0].regionId, before);
    EXPECT_NEAR (after.regions[0].area, 100 - (4 - kPi) * 3.5 * 3.5, 0.3);
}

TEST (EditLandscapeFillet, AStrokeAlongARoundedOpenGuideFollowsItsArcs)
{
    // An open guide 0,0 -> 10,0 -> 10,10 -> 0,10 rounded at its two inner corners, stroked
    // 1 m wide: the strip's area is its centreline length times the width.
    const double radius = 2.0;
    auto r = Square (radius, false);
    const auto result = Evaluate (r.doc);
    ASSERT_EQ (result.regions.size (), 1u);
    const double centreline = 3 * 10 - 2 * (2 * radius) + 2 * (kPi / 2 * radius);
    const double strip = centreline * 1.5 + kPi * 0.75 * 0.75; // width 1.5, round caps
    EXPECT_NEAR (result.regions[0].area, strip, 0.1);
}
