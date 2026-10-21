// Generated boundary pieces (offset sides and joins, stroke sides) carry the exact source
// subdomain they descend from, and lineage moves them with that subdomain instead of leaving
// them where they were.
#include "Edit/EditDocument.hpp"
#include "Edit/EditLandscape.hpp"
#include "Edit/EditSession.hpp"
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>

namespace edit = geomsrv::edit;
namespace geo = tapioca::geometry;

namespace {
edit::Document Start ()
{
    edit::Document doc;
    std::string error;
    EXPECT_TRUE (geo::StartLandscape (doc, { 7 }, error)) << error;
    return doc;
}
edit::Id Source (edit::Document& doc, const std::vector<edit::Vec3>& points, bool closed)
{
    geo::LandscapeSource source;
    source.entityId = edit::AddPolyline (doc, points, closed);
    edit::Id id = 0;
    std::string error;
    EXPECT_TRUE (geo::AddLandscapeSource (doc, source, id, error)) << error;
    return id;
}
edit::Id Square (edit::Document& doc, double x, double y, double size)
{
    return Source (doc, { { x, y, 0 }, { x + size, y, 0 }, { x + size, y + size, 0 }, { x, y + size, 0 } }, true);
}
edit::Id Operation (edit::Document& doc, geo::OperationKind kind, std::vector<edit::Id> inputs, double amount = 0)
{
    geo::LandscapeOperation op;
    op.kind = kind;
    op.inputs = std::move (inputs);
    if (kind == geo::OperationKind::Offset)
        op.offset = amount;
    if (kind == geo::OperationKind::Stroke)
        op.width = amount;
    edit::Id id = 0;
    std::string error;
    EXPECT_TRUE (geo::AddLandscapeOperation (doc, op, id, error)) << error;
    return id;
}
void Object (edit::Document& doc, edit::Id op)
{
    geo::HardscapeObject object;
    object.footprintOperation = op;
    edit::Id id = 0;
    std::string error;
    EXPECT_TRUE (geo::AddHardscapeObject (doc, object, id, error)) << error;
}
geo::LandscapeResult Evaluate (const edit::Document& doc)
{
    geo::LandscapeResult result;
    EXPECT_TRUE (geo::EvaluateLandscape (doc, result)) << result.error;
    return result;
}
// Each parameter names a source piece the piece's support also names.
void ExpectSupported (const edit::Document& doc, const geo::RegionBoundaryPiece& piece)
{
    for (const auto& p : piece.parameters) {
        const auto source = std::find_if (doc.landscape->sources.begin (), doc.landscape->sources.end (),
                                          [&] (const auto& s) { return s.id == p.source; });
        ASSERT_NE (source, doc.landscape->sources.end ());
        const auto& segments = edit::Find (doc, source->entityId)->segments;
        ASSERT_LT (p.piece, segments.size ());
        EXPECT_TRUE (std::any_of (piece.support.begin (), piece.support.end (), [&] (const auto& ref) {
            return ref.source == p.source && ref.segment == segments[p.piece].id;
        })) << "a subdomain on a segment the piece is not supported by";
        EXPECT_LE (p.sourceStart, p.sourceEnd);
    }
}
} // namespace

TEST (EditLandscapeGenerated, OffsetSidesSpanTheirSegmentAndJoinsSitOnTheirCorner)
{
    auto doc = Start ();
    Object (doc, Operation (doc, geo::OperationKind::Offset, { Square (doc, 0, 0, 10) }, 1));
    const auto result = Evaluate (doc);
    ASSERT_EQ (result.regions.size (), 1u);
    size_t sides = 0, joins = 0;
    for (const auto& piece : result.regions[0].boundary[0]) {
        ASSERT_TRUE (piece.generated);
        ASSERT_FALSE (piece.parameters.empty ()) << "a generated piece names the subdomain it came from";
        ExpectSupported (doc, piece);
        const auto m = edit::Lerp (piece.a, piece.b, 0.5);
        const bool side = (m.x > 0 && m.x < 10) || (m.y > 0 && m.y < 10);
        if (side) {
            ++sides;
            ASSERT_EQ (piece.parameters.size (), 1u);
            EXPECT_GT (piece.parameters[0].sourceEnd - piece.parameters[0].sourceStart, 0.0);
        }
        else {
            ++joins;
            EXPECT_EQ (piece.parameters.size (), 2u) << "a join descends from both legs' shared corner";
            for (const auto& p : piece.parameters)
                EXPECT_EQ (p.sourceStart, p.sourceEnd) << "a corner is a point of each leg";
        }
    }
    EXPECT_GE (sides, 4u);
    EXPECT_GE (joins, 4u);
}

TEST (EditLandscapeGenerated, StrokeSidesCarryTheirCentrelineSubdomain)
{
    auto doc = Start ();
    Object (doc, Operation (doc, geo::OperationKind::Stroke,
                            { Source (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 } }, false) }, 2));
    const auto result = Evaluate (doc);
    ASSERT_EQ (result.regions.size (), 1u);
    size_t spans = 0;
    for (const auto& piece : result.regions[0].boundary[0]) {
        ASSERT_FALSE (piece.parameters.empty ());
        ExpectSupported (doc, piece);
        const auto m = edit::Lerp (piece.a, piece.b, 0.5);
        // The long sides of the first leg, clear of both ends.
        if (m.x > 2 && m.x < 8 && std::abs (std::abs (m.y) - 1) < 1.0e-6) {
            ++spans;
            ASSERT_EQ (piece.parameters.size (), 1u);
            EXPECT_GT (piece.parameters[0].sourceEnd - piece.parameters[0].sourceStart, 0.0);
        }
    }
    EXPECT_GE (spans, 2u);
}

TEST (EditLandscapeGenerated, AnOffsetFootprintMovedClearOfItselfKeepsItsLineage)
{
    // One edit carries the offset square 40 m away and cuts a hole into it: a new region
    // whose only possible predecessor no longer overlaps its old place.
    auto doc = Start ();
    const auto square = Square (doc, 0, 0, 10), notch = Square (doc, -100, 0, 2);
    const auto grown = Operation (doc, geo::OperationKind::Offset, { square }, 1);
    Object (doc, Operation (doc, geo::OperationKind::Difference, { grown, notch }));
    const auto initial = Evaluate (doc);
    ASSERT_EQ (initial.regions.size (), 1u);
    const auto original = initial.regions[0].regionId;
    const auto squareEntity = doc.landscape->sources[0].entityId, notchEntity = doc.landscape->sources[1].entityId;
    edit::Session session (doc);
    ASSERT_TRUE (session.Apply ([=] (edit::Document& d, std::string&) {
        for (const auto& [entity, by] : { std::pair { squareEntity, 40.0 }, std::pair { notchEntity, 144.0 } })
            for (auto& v : edit::Find (d, entity)->vertices)
                v.p = { v.p.x + by, v.p.y + 4, v.p.z };
        return true;
    })) << session.Message ();
    const auto after = Evaluate (session.Doc ());
    ASSERT_EQ (after.regions.size (), 1u);
    ASSERT_EQ (after.regions[0].rings.size (), 2u) << "the notch is now a hole";
    EXPECT_NE (after.regions[0].regionId, original);
    EXPECT_EQ (after.regions[0].predecessorRegions, (std::vector<edit::Id> { original }));
}
