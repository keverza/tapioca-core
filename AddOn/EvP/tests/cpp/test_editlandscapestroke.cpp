#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPolyline.hpp"
#include "Edit/EditSession.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <set>

namespace geo = tapioca::geometry;
namespace edit = geomsrv::edit;
namespace {
constexpr double kPi = 3.14159265358979323846;
edit::Document Source (const std::vector<edit::Vec3>& points, bool closed = false, const std::vector<double>& arcs = {})
{
    edit::Document doc;
    std::string error;
    EXPECT_TRUE (geo::StartLandscape (doc, { 1 }, error)) << error;
    geo::LandscapeSource source;
    source.entityId = edit::AddPolyline (doc, points, closed);
    source.arcAngles = arcs;
    edit::Id id = 0;
    EXPECT_TRUE (geo::AddLandscapeSource (doc, source, id, error)) << error;
    return doc;
}
void Stroke (edit::Document& doc, double width)
{
    geo::LandscapeOperation op;
    op.kind = geo::OperationKind::Stroke;
    op.inputs = { doc.landscape->sources[0].id };
    op.width = width;
    edit::Id id = 0;
    std::string error;
    ASSERT_TRUE (geo::AddLandscapeOperation (doc, op, id, error)) << error;
    geo::HardscapeObject object;
    object.footprintOperation = id;
    ASSERT_TRUE (geo::AddHardscapeObject (doc, object, id, error)) << error;
}
geo::LandscapeResult Evaluate (const edit::Document& doc)
{
    geo::LandscapeResult result;
    EXPECT_TRUE (geo::EvaluateLandscape (doc, result)) << result.error;
    return result;
}
} // namespace

TEST (EditLandscapeStroke, ClosedStrokeRetainsItsHoleAndLocalSegmentOwners)
{
    auto doc = Source ({ { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } }, true);
    Stroke (doc, 2);
    const auto result = Evaluate (doc);
    ASSERT_EQ (result.regions.size (), 1u);
    const auto& region = result.regions[0];
    ASSERT_EQ (region.rings.size (), 2u);
    EXPECT_NEAR (region.area, 76 + kPi, 0.015);
    for (const auto& cycle : region.boundary)
        for (const auto& piece : cycle) {
            const auto mid = edit::Lerp (piece.a, piece.b, 0.5);
            if (mid.x > 2 && mid.x < 8 && (std::abs (mid.y + 1) < 1.0e-6 || std::abs (mid.y - 1) < 1.0e-6)) {
                ASSERT_EQ (piece.support.size (), 1u);
                EXPECT_EQ (piece.support[0].segment, doc.entities[0].segments[0].id);
            }
        }
}

TEST (EditLandscapeStroke, QuarterArcRetainsAnalyticTubeAreaAndItsAuthoredSegment)
{
    auto doc = Source ({ { 3, 0, 0 }, { 0, 3, 0 } }, false, { kPi / 2, 0 });
    Stroke (doc, 1);
    const auto result = Evaluate (doc);
    ASSERT_EQ (result.regions.size (), 1u);
    EXPECT_NEAR (result.regions[0].area, 1.5 * kPi + 0.25 * kPi, 0.015);
    for (const auto& cycle : result.regions[0].boundary)
        for (const auto& piece : cycle) {
            ASSERT_FALSE (piece.support.empty ());
            EXPECT_TRUE (piece.generated);
            EXPECT_TRUE (piece.parameters.empty ());
            for (const auto& support : piece.support) {
                EXPECT_EQ (support.source, doc.landscape->sources[0].id);
                EXPECT_EQ (support.segment, doc.entities[0].segments[0].id);
            }
        }
}

TEST (EditLandscapeStroke, CubicTubeFollowsTheCurveInsteadOfItsAnchorChord)
{
    auto doc = Source ({ { 0, 0, 0 }, { 10, 0, 0 } });
    doc.entities[0].vertices[0].out = { 0, 5, 0 };
    doc.entities[0].vertices[1].in = { 0, 5, 0 };
    Stroke (doc, 1);
    const auto result = Evaluate (doc);
    ASSERT_EQ (result.regions.size (), 1u);
    double maxY = 0;
    for (const auto& cycle : result.regions[0].boundary)
        for (const auto& piece : cycle) {
            maxY = std::max ({ maxY, piece.a.y, piece.b.y });
            for (const auto& support : piece.support)
                EXPECT_EQ (support.segment, doc.entities[0].segments[0].id);
        }
    // This cubic's analytic apex is y=3.75, plus the half-width 0.5.
    EXPECT_NEAR (maxY, 4.25, 0.003);
}

TEST (EditLandscapeStroke, BentStrokeReloadAndEndpointEditPreserveSemanticIdentity)
{
    auto doc = Source ({ { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 } });
    Stroke (doc, 2);
    const auto original = Evaluate (doc);
    ASSERT_EQ (original.regions.size (), 1u);
    std::string text, error;
    ASSERT_TRUE (geo::WriteLandscape (doc, geo::LandscapeStorage::Virtual, text, error)) << error;
    edit::Document loaded;
    ASSERT_TRUE (geo::ReadLandscape (text, loaded, error)) << error;
    EXPECT_EQ (loaded.landscape, doc.landscape);
    const auto entity = loaded.entities[0].id, vertex = loaded.entities[0].vertices[2].id;
    edit::Session session (loaded);
    ASSERT_TRUE (session.Apply ([=] (edit::Document& d, std::string& e) {
        return edit::MoveVertex (d, entity, vertex, { 10, 12, 0 }, e);
    })) << session.Message ();
    const auto after = Evaluate (session.Doc ());
    ASSERT_EQ (after.regions.size (), 1u);
    EXPECT_EQ (after.regions[0].regionId, original.regions[0].regionId);
    EXPECT_EQ (after.regions[0].descriptor, original.regions[0].descriptor);
    EXPECT_NEAR (after.regions[0].area, 43 + 1.25 * kPi, 0.015);
    ASSERT_TRUE (session.Undo ());
    EXPECT_EQ (session.Doc ().landscape, doc.landscape);
    ASSERT_TRUE (session.Redo ());
    ASSERT_TRUE (geo::WriteLandscape (session.Doc (), geo::LandscapeStorage::Virtual, text, error)) << error;
    ASSERT_TRUE (geo::ReadLandscape (text, loaded, error)) << error;
    EXPECT_EQ (loaded.landscape, session.Doc ().landscape);
}

TEST (EditLandscapeStroke, CapsuleBudgetRefusesWithoutPublishingAnOperationOrConsumingIds)
{
    std::vector<edit::Vec3> points;
    for (int i = 0; i <= 700; ++i)
        points.push_back ({ double (i), double (i % 2) * 2, 0 });
    auto doc = Source (points);
    const auto before = doc;
    geo::LandscapeOperation op;
    op.kind = geo::OperationKind::Stroke;
    op.inputs = { doc.landscape->sources[0].id };
    edit::Id id = 123;
    std::string error;
    EXPECT_FALSE (geo::AddLandscapeOperation (doc, op, id, error));
    EXPECT_NE (error.find ("capsule boundary segment budget"), std::string::npos) << error;
    EXPECT_EQ (id, 123u);
    EXPECT_EQ (doc.landscape, before.landscape);
    EXPECT_EQ (doc.nextId, before.nextId);
    EXPECT_EQ (doc.revision, before.revision);
}

TEST (EditLandscapeStroke, HugeCapsRefuseBeforeInflationWithoutChangingTheDocument)
{
    for (double width : { 1.0e9, 1.0e100 }) {
        auto doc = Source ({ { 0, 0, 0 }, { 10, 0, 0 } });
        const auto before = doc;
        geo::LandscapeOperation op;
        op.kind = geo::OperationKind::Stroke;
        op.inputs = { doc.landscape->sources[0].id };
        op.width = width;
        edit::Id id = 123;
        std::string error;
        EXPECT_FALSE (geo::AddLandscapeOperation (doc, op, id, error));
        EXPECT_NE (error.find ("capsule boundary segment budget"), std::string::npos) << error;
        EXPECT_EQ (id, 123u);
        EXPECT_EQ (doc.landscape, before.landscape);
        EXPECT_EQ (doc.nextId, before.nextId);
        EXPECT_EQ (doc.revision, before.revision);
    }
}

TEST (EditLandscapeStroke, CrossedStrokeKeepsAHoleWithoutTaggingAllSegmentsOnEveryEdge)
{
    auto doc = Source ({ { 0, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 }, { 10, 0, 0 } });
    Stroke (doc, 1);
    const auto result = Evaluate (doc);
    ASSERT_EQ (result.regions.size (), 1u);
    ASSERT_EQ (result.regions[0].rings.size (), 2u);
    bool rising = false, falling = false;
    for (const auto& cycle : result.regions[0].boundary)
        for (const auto& piece : cycle) {
            const auto mid = edit::Lerp (piece.a, piece.b, 0.5);
            if (mid.x > 1 && mid.x < 3 && mid.y > 1 && mid.y < 3) {
                rising = true;
                ASSERT_EQ (piece.support.size (), 1u);
                EXPECT_EQ (piece.support[0].segment, doc.entities[0].segments[0].id);
            }
            if (mid.x > 7 && mid.x < 9 && mid.y > 1 && mid.y < 3) {
                falling = true;
                ASSERT_EQ (piece.support.size (), 1u);
                EXPECT_EQ (piece.support[0].segment, doc.entities[0].segments[2].id);
            }
        }
    EXPECT_TRUE (rising);
    EXPECT_TRUE (falling);
}

namespace {
// The authored segments (path legs for a path) a boundary piece claims.
std::set<edit::Id> Segments (const geo::RegionBoundaryPiece& piece)
{
    std::set<edit::Id> segments;
    for (const auto& support : piece.support)
        segments.insert (support.segment);
    return segments;
}
void Offset (edit::Document& doc, double distance)
{
    geo::LandscapeOperation op;
    op.kind = geo::OperationKind::Offset;
    op.inputs = { doc.landscape->sources[0].id };
    op.offset = distance;
    edit::Id id = 0;
    std::string error;
    ASSERT_TRUE (geo::AddLandscapeOperation (doc, op, id, error)) << error;
    geo::HardscapeObject object;
    object.footprintOperation = id;
    ASSERT_TRUE (geo::AddHardscapeObject (doc, object, id, error)) << error;
}
} // namespace

TEST (EditLandscapeOffset, GrownEdgesKeepTheirParallelSegmentAndJoinsTheirCorner)
{
    auto doc = Source ({ { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } }, true);
    Offset (doc, 1);
    const auto result = Evaluate (doc);
    ASSERT_EQ (result.regions.size (), 1u);
    EXPECT_NEAR (result.regions[0].area, 140 + kPi, 0.015);
    const auto& s = doc.entities[0].segments;
    size_t sides = 0, joins = 0;
    for (const auto& cycle : result.regions[0].boundary)
        for (const auto& piece : cycle) {
            EXPECT_TRUE (piece.generated);
            const auto m = edit::Lerp (piece.a, piece.b, 0.5);
            const bool inX = m.x > 0 && m.x < 10, inY = m.y > 0 && m.y < 10;
            std::set<edit::Id> expected;
            if (inX || inY) {
                ++sides;
                expected = { inX ? (m.y < 5 ? s[0].id : s[2].id) : (m.x > 5 ? s[1].id : s[3].id) };
            }
            else {
                ++joins;
                // Segment i runs from vertex i: the corner at vertex k joins k-1 and k.
                const size_t k = m.x > 5 ? (m.y > 5 ? 2 : 1) : (m.y > 5 ? 3 : 0);
                expected = { s[k].id, s[(k + 3) % 4].id };
            }
            EXPECT_EQ (Segments (piece), expected) << m.x << "," << m.y;
        }
    EXPECT_GE (sides, 4u);
    EXPECT_GE (joins, 4u);
}

TEST (EditLandscapeOffset, InsetEdgesKeepOnlyTheirParallelSegment)
{
    auto doc = Source ({ { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } }, true);
    Offset (doc, -2);
    const auto result = Evaluate (doc);
    ASSERT_EQ (result.regions.size (), 1u);
    EXPECT_NEAR (result.regions[0].area, 36, 1.0e-4);
    const auto& s = doc.entities[0].segments;
    for (const auto& cycle : result.regions[0].boundary)
        for (const auto& piece : cycle) {
            const auto m = edit::Lerp (piece.a, piece.b, 0.5);
            const auto side = std::abs (m.y - 2) < 1.0e-6   ? s[0].id
                              : std::abs (m.x - 8) < 1.0e-6 ? s[1].id
                              : std::abs (m.y - 8) < 1.0e-6 ? s[2].id
                                                            : s[3].id;
            EXPECT_EQ (Segments (piece), (std::set<edit::Id> { side })) << m.x << "," << m.y;
        }
}

TEST (EditLandscapeStroke, PathOutlineSidesKeepTheirOwnCentrelineLeg)
{
    edit::Document doc;
    std::string error;
    ASSERT_TRUE (geo::StartLandscape (doc, { 1 }, error)) << error;
    const auto path = edit::AddPath (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 } }, 2, "walk");
    geo::LandscapeSource source;
    source.pathId = path;
    source.role = geo::SourceRole::PathCenterline;
    edit::Id id = 0;
    ASSERT_TRUE (geo::AddLandscapeSource (doc, source, id, error)) << error;
    geo::LandscapeOperation op;
    op.kind = geo::OperationKind::Stroke;
    op.inputs = { id };
    ASSERT_TRUE (geo::AddLandscapeOperation (doc, op, id, error)) << error;
    geo::HardscapeObject object;
    object.footprintOperation = id;
    ASSERT_TRUE (geo::AddHardscapeObject (doc, object, id, error)) << error;
    const auto result = Evaluate (doc);
    ASSERT_EQ (result.regions.size (), 1u);
    const auto& points = edit::FindPath (doc, path)->points;
    size_t first = 0, second = 0;
    for (const auto& cycle : result.regions[0].boundary)
        for (const auto& piece : cycle) {
            const auto m = edit::Lerp (piece.a, piece.b, 0.5);
            if (m.x > 2 && m.x < 8 && std::abs (std::abs (m.y) - 1) < 1.0e-3) {
                ++first;
                EXPECT_EQ (Segments (piece), (std::set<edit::Id> { points[0].id })) << m.x << "," << m.y;
            }
            if (m.y > 2 && m.y < 8 && std::abs (std::abs (m.x - 10) - 1) < 1.0e-3) {
                ++second;
                EXPECT_EQ (Segments (piece), (std::set<edit::Id> { points[1].id })) << m.x << "," << m.y;
            }
        }
    EXPECT_GE (first, 2u);
    EXPECT_GE (second, 2u);
}
