#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditPolyline.hpp"
#include "Edit/EditSession.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>

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
