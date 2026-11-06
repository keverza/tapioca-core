// Closed-entity booleans (EditBoolean.hpp): union, subtract, intersect and exclude with holes and
// islands, the first operand's identity kept, and every refusal atomic.
#include "Edit/EditBoolean.hpp"
#include "Edit/EditLandscape.hpp"
#include "Edit/EditOrganize.hpp"
#include "Edit/EditSession.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include <gtest/gtest.h>
#include <array>
#include <cmath>

namespace ed = geomsrv::edit;
namespace geo = tapioca::geometry;

namespace {

std::vector<ed::Vec3> Square (double x0, double y0, double x1, double y1, double z = 0.0)
{
    return { { x0, y0, z }, { x1, y0, z }, { x1, y1, z }, { x0, y1, z } };
}

ed::Id Box (ed::Document& doc, double x0, double y0, double x1, double y1, const std::string& role = {})
{
    return ed::AddPolyline (doc, Square (x0, y0, x1, y1), true, role);
}

double Area (const ed::Document& doc, const std::vector<ed::Id>& ids)
{
    double area = 0.0;
    for (const auto id : ids)
        area += ed::SignedAreaXY (*ed::Find (doc, id));
    return area;
}

bool Apply (ed::Document& doc, const std::vector<ed::Id>& operands, ed::BooleanKind kind, std::vector<ed::Id>& made,
            std::string& error)
{
    return ed::BooleanEntities (doc, operands, kind, error, &made);
}

} // namespace

TEST (EditBoolean, UnionKeepsTheFirstOperandAndRemovesTheRest)
{
    ed::Document doc;
    const ed::Id a = Box (doc, 0, 0, 4, 4, "platform");
    const ed::Id b = Box (doc, 2, 2, 6, 6, "other");
    const ed::Id layer = ed::AddLayer (doc, "paving");
    std::string error;
    ASSERT_TRUE (ed::MoveToLayer (doc, { { a, ed::Part::Entity, 0 } }, layer, error)) << error;
    std::vector<ed::Id> made;
    ASSERT_TRUE (Apply (doc, { a, b }, ed::BooleanKind::Union, made, error)) << error;
    ASSERT_EQ (made, std::vector<ed::Id> { a });
    EXPECT_EQ (ed::Find (doc, b), nullptr);
    const ed::Entity& e = *ed::Find (doc, a);
    EXPECT_EQ (e.role, "platform");
    EXPECT_EQ (e.member.layer, layer);
    EXPECT_FALSE (ed::IsCompound (e));
    EXPECT_NEAR (ed::SignedAreaXY (e), 28.0, 1e-9);
    EXPECT_TRUE (ed::Validate (doc, error)) << error;
}

TEST (EditBoolean, SubtractInsideMakesAHoleAndOrderMatters)
{
    ed::Document doc;
    const ed::Id outer = Box (doc, 0, 0, 10, 10);
    const ed::Id inner = Box (doc, 3, 3, 7, 7);
    const ed::Document before = doc;
    std::vector<ed::Id> made;
    std::string error;
    ASSERT_TRUE (Apply (doc, { outer, inner }, ed::BooleanKind::Subtract, made, error)) << error;
    const ed::Entity& e = *ed::Find (doc, outer);
    ASSERT_TRUE (ed::IsCompound (e));
    EXPECT_EQ (e.contours.size (), 2u);
    EXPECT_NEAR (ed::SignedAreaXY (e), 84.0, 1e-9);
    EXPECT_FALSE (ed::InsideXY (e, { 5, 5, 0 }));
    EXPECT_TRUE (ed::Validate (doc, error)) << error;

    // The small one first: nothing of it is left outside the big one.
    ed::Document reversed = before;
    EXPECT_FALSE (Apply (reversed, { inner, outer }, ed::BooleanKind::Subtract, made, error));
    EXPECT_NE (error.find ("empty"), std::string::npos);
    EXPECT_EQ (reversed.entities, before.entities);
}

TEST (EditBoolean, IntersectAndExclude)
{
    ed::Document doc;
    const ed::Id a = Box (doc, 0, 0, 4, 4), b = Box (doc, 2, 2, 6, 6), c = Box (doc, 3, 0, 5, 5);
    const ed::Document start = doc;
    std::vector<ed::Id> made;
    std::string error;
    ASSERT_TRUE (Apply (doc, { a, b, c }, ed::BooleanKind::Intersect, made, error)) << error;
    EXPECT_NEAR (Area (doc, made), 2.0, 1e-9); // x 3..4, y 2..4
    EXPECT_EQ (doc.entities.size (), 1u);

    doc = start;
    ASSERT_TRUE (Apply (doc, { a, b }, ed::BooleanKind::Exclude, made, error)) << error;
    EXPECT_NEAR (Area (doc, made), 32.0 - 8.0, 1e-9);
    EXPECT_EQ (made.front (), a);
    EXPECT_TRUE (ed::Validate (doc, error)) << error;

    // Nested: the overlap is the inner one, so what is left is a ring.
    ed::Document nested;
    const ed::Id big = Box (nested, 0, 0, 10, 10), small = Box (nested, 3, 3, 7, 7);
    ASSERT_TRUE (Apply (nested, { big, small }, ed::BooleanKind::Exclude, made, error)) << error;
    EXPECT_TRUE (ed::IsCompound (*ed::Find (nested, big)));
    EXPECT_NEAR (Area (nested, made), 84.0, 1e-9);
}

TEST (EditBoolean, HolesAndIslandsSurviveEveryOperation)
{
    ed::Document doc;
    const ed::Id donut = ed::AddCompound (doc, { Square (0, 0, 10, 10), Square (2, 2, 8, 8) }, ed::Kind::Polyline, 0);
    const ed::Id island = Box (doc, 4, 4, 6, 6);
    std::vector<ed::Id> made;
    std::string error;
    // An operand's hole is a hole: the island lands in it, not on solid.
    ASSERT_TRUE (Apply (doc, { donut, island }, ed::BooleanKind::Union, made, error)) << error;
    const ed::Entity& e = *ed::Find (doc, donut);
    ASSERT_EQ (e.contours.size (), 3u);
    EXPECT_NEAR (ed::SignedAreaXY (e), 100.0 - 36.0 + 4.0, 1e-9);
    EXPECT_TRUE (ed::InsideXY (e, { 5, 5, 0 }));
    EXPECT_FALSE (ed::InsideXY (e, { 3, 5, 0 }));
    EXPECT_EQ (ed::ContourDepth (e, 2), 2u);
    EXPECT_TRUE (ed::Validate (doc, error)) << error;

    // A square over the ring's hole meets only the ring's solid band and the island.
    const ed::Id cut = Box (doc, 1, 1, 9, 9);
    ASSERT_TRUE (Apply (doc, { donut, cut }, ed::BooleanKind::Intersect, made, error)) << error;
    EXPECT_NEAR (Area (doc, made), 64.0 - 36.0 + 4.0, 1e-9);
    EXPECT_TRUE (ed::Validate (doc, error)) << error;
}

TEST (EditBoolean, ASplitResultKeepsTheLargestPieceOnTheFirstId)
{
    ed::Document doc;
    const ed::Id slab = Box (doc, 0, 0, 10, 4, "slab");
    const ed::Id bar = Box (doc, 6, -1, 7, 5);
    std::vector<ed::Id> made;
    std::string error;
    ASSERT_TRUE (Apply (doc, { slab, bar }, ed::BooleanKind::Subtract, made, error)) << error;
    ASSERT_EQ (made.size (), 2u);
    EXPECT_EQ (made[0], slab);
    EXPECT_NEAR (ed::SignedAreaXY (*ed::Find (doc, made[0])), 24.0, 1e-9); // x 0..6
    EXPECT_NEAR (ed::SignedAreaXY (*ed::Find (doc, made[1])), 12.0, 1e-9); // x 7..10
    EXPECT_EQ (ed::Find (doc, made[1])->role, "slab");
    EXPECT_EQ (ed::Find (doc, bar), nullptr);
    EXPECT_TRUE (ed::Validate (doc, error)) << error;
}

TEST (EditBoolean, SolidsKeepTheFirstHeightAndBase)
{
    ed::Document doc;
    const ed::Id a = ed::AddExtrusion (doc, Square (0, 0, 10, 10, 1.0), 2.5);
    const ed::Id b = ed::AddExtrusion (doc, Square (3, 3, 7, 7, 1.0), 9.0);
    std::vector<ed::Id> made;
    std::string error;
    ASSERT_TRUE (Apply (doc, { a, b }, ed::BooleanKind::Subtract, made, error)) << error;
    const ed::Entity& e = *ed::Find (doc, a);
    EXPECT_EQ (e.kind, ed::Kind::Extrusion);
    EXPECT_DOUBLE_EQ (e.height, 2.5);
    for (const auto& v : e.vertices)
        EXPECT_DOUBLE_EQ (v.p.z, 1.0);
    EXPECT_EQ (ed::FaceCount (e), 2u + 8u);
    EXPECT_TRUE (ed::Validate (doc, error)) << error;
}

TEST (EditBoolean, CurvesAreFlattenedWithinTheDocumentedTolerance)
{
    ed::Document doc;
    const ed::Id a = Box (doc, -5, -5, 5, 5);
    const ed::Id circle = Box (doc, -1, -1, 1, 1);
    std::string error;
    for (auto& v : ed::Find (doc, circle)->vertices)
        v.radius = 1.0; // every corner rounded: a circle of radius 1
    ASSERT_TRUE (ed::Validate (doc, error)) << error;
    std::vector<ed::Id> made;
    ASSERT_TRUE (Apply (doc, { a, circle }, ed::BooleanKind::Subtract, made, error)) << error;
    const ed::Entity& e = *ed::Find (doc, a);
    ASSERT_TRUE (ed::IsCompound (e));
    // The hole is straight pieces turning at most kBooleanMaxAngle: its area is within the
    // inscribed polygon's shortfall of pi.
    const double hole = 100.0 - ed::SignedAreaXY (e);
    EXPECT_LT (hole, 3.14159265358979);
    EXPECT_GT (hole, 3.14159265358979 * (1.0 - ed::kBooleanMaxAngle * ed::kBooleanMaxAngle));
    for (const auto& v : e.vertices) {
        EXPECT_DOUBLE_EQ (v.radius, 0.0);
        EXPECT_DOUBLE_EQ (v.in.x, 0.0);
    }
}

TEST (EditBoolean, InvalidInputsRefuseAtomically)
{
    ed::Document doc;
    const ed::Id a = Box (doc, 0, 0, 4, 4), b = Box (doc, 2, 2, 6, 6), far = Box (doc, 20, 20, 22, 22);
    const ed::Id open = ed::AddPolyline (doc, { { 0, 0, 0 }, { 5, 5, 0 } }, false);
    const ed::Id high = ed::AddPolyline (doc, Square (1, 1, 3, 3, 2.0), true);
    const ed::Id solid = ed::AddExtrusion (doc, Square (1, 1, 3, 3), 1.0);
    const ed::Document before = doc;
    std::vector<ed::Id> made;
    std::string error;
    const auto refused = [&] (const std::vector<ed::Id>& operands, ed::BooleanKind kind, const char* why) {
        ed::Document copy = before;
        EXPECT_FALSE (Apply (copy, operands, kind, made, error)) << why;
        EXPECT_NE (error.find (why), std::string::npos) << error;
        EXPECT_EQ (copy.entities, before.entities) << why;
        EXPECT_EQ (copy.nextId, before.nextId) << why;
    };
    refused ({ a }, ed::BooleanKind::Union, "at least two");
    refused ({ a, a }, ed::BooleanKind::Union, "twice");
    refused ({ a, open }, ed::BooleanKind::Union, "closed");
    refused ({ a, high }, ed::BooleanKind::Union, "plane");
    refused ({ a, solid }, ed::BooleanKind::Union, "solids");
    refused ({ a, far }, ed::BooleanKind::Intersect, "empty");
    refused ({ a, 9999 }, ed::BooleanKind::Union, "no longer exists");

    ed::Document locked = before;
    ASSERT_TRUE (ed::SetLocked (locked, { { b, ed::Part::Entity, 0 } }, true, error)) << error;
    const auto lockedBefore = locked.entities;
    EXPECT_FALSE (Apply (locked, { a, b }, ed::BooleanKind::Union, made, error));
    EXPECT_NE (error.find ("locked"), std::string::npos);
    EXPECT_EQ (locked.entities, lockedBefore);
}

TEST (EditBoolean, LandscapeSourcesAreRefused)
{
    ed::Document doc;
    std::string error;
    ASSERT_TRUE (geo::StartLandscape (doc, { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, error));
    const ed::Id a = Box (doc, 0, 0, 4, 4), b = Box (doc, 2, 2, 6, 6);
    ASSERT_TRUE (geo::SetLandscapeSourceRole (doc, b, geo::SourceRole::Guide, error)) << error;
    const auto before = doc.entities;
    std::vector<ed::Id> made;
    EXPECT_FALSE (Apply (doc, { a, b }, ed::BooleanKind::Union, made, error));
    EXPECT_NE (error.find ("landscape"), std::string::npos);
    EXPECT_EQ (doc.entities, before);
}

TEST (EditBoolean, OneUndoRestoresEveryOperand)
{
    ed::Document doc;
    const ed::Id a = Box (doc, 0, 0, 10, 10), b = Box (doc, 3, 3, 7, 7);
    ed::Session session (doc);
    const auto before = session.Doc ().entities;
    ASSERT_TRUE (session.Apply ([&] (ed::Document& d, std::string& error) {
        return ed::BooleanEntities (d, { a, b }, ed::BooleanKind::Subtract, error);
    })) << session.Message ();
    EXPECT_EQ (session.UndoDepth (), 1u);
    EXPECT_EQ (session.Doc ().entities.size (), 1u);
    ASSERT_TRUE (session.Undo ());
    EXPECT_EQ (session.Doc ().entities, before);
    ASSERT_TRUE (session.Redo ());
    EXPECT_TRUE (ed::IsCompound (*ed::Find (session.Doc (), a)));
}

// #158: the editor's default drawing role is Reference line, so a boolean of what was just drawn
// takes editor-owned reference lines and keeps their source records coherent in the same edit.
namespace {

const std::array<uint8_t, 16> kSession { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

struct ReferenceScene {
    ed::Document doc;
    ed::Id a = ed::kNoId, b = ed::kNoId;
    std::string error;

    ReferenceScene (double bx0, double by0, double bx1, double by1)
    {
        EXPECT_TRUE (geo::StartLandscape (doc, kSession, error));
        a = Box (doc, 0, 0, 10, 10);
        b = Box (doc, bx0, by0, bx1, by1);
        EXPECT_TRUE (geo::SetLandscapeSourceRole (doc, a, geo::SourceRole::ReferenceLine, error)) << error;
        EXPECT_TRUE (geo::SetLandscapeSourceRole (doc, b, geo::SourceRole::ReferenceLine, error)) << error;
    }
};

const geo::LandscapeSource* SourceById (const ed::Document& doc, ed::Id id)
{
    for (const auto& s : doc.landscape->sources)
        if (s.id == id)
            return &s;
    return nullptr;
}

const geo::LandscapeSource* LiveSource (const ed::Document& doc, ed::Id entity)
{
    for (const auto& s : doc.landscape->sources)
        if (!s.deleted && s.entityId == entity)
            return &s;
    return nullptr;
}

} // namespace

TEST (EditBoolean, EditorOwnedReferenceLinesSubtractIntoAHoleAndKeepCoherentSources)
{
    ReferenceScene scene (3, 3, 7, 7);
    const auto& doc = scene.doc;
    geo::ReferenceLineStyle style;
    style.rgba = 0x11223344;
    style.widthPixels = 2.5;
    const ed::Id keptSource = LiveSource (doc, scene.a)->id, goneSource = LiveSource (doc, scene.b)->id;
    ASSERT_TRUE (geo::SetReferenceLineStyle (scene.doc, keptSource, style, scene.error)) << scene.error;
    ed::Session session (doc);
    ASSERT_TRUE (session.Apply ([&] (ed::Document& d, std::string& error) {
        return ed::BooleanEntities (d, { scene.a, scene.b }, ed::BooleanKind::Subtract, error);
    })) << session.Message ();
    const ed::Document after = session.Doc ();
    ASSERT_EQ (after.entities.size (), 1u);
    EXPECT_TRUE (ed::IsCompound (*ed::Find (after, scene.a)));
    // The kept outline's source is the same record, still a reference line of its style.
    const auto* kept = SourceById (after, keptSource);
    const auto* gone = SourceById (after, goneSource);
    ASSERT_NE (kept, nullptr);
    ASSERT_NE (gone, nullptr);
    EXPECT_FALSE (kept->deleted);
    EXPECT_EQ (kept->entityId, scene.a);
    EXPECT_EQ (kept->role, geo::SourceRole::ReferenceLine);
    EXPECT_EQ (kept->lineStyle, style);
    EXPECT_TRUE (gone->deleted) << "a removed operand's source must be a tombstone, not a dangling record";
    std::string error;
    EXPECT_TRUE (ed::Validate (after, error)) << error;
    // Linework only: nothing in the landscape evaluates it.
    EXPECT_TRUE (after.landscape->operations.empty ());
    EXPECT_TRUE (after.landscape->objects.empty ());
    // The virtual codec round-trips the compound reference line and the tombstone.
    std::string json;
    ASSERT_TRUE (geo::WriteLandscape (after, geo::LandscapeStorage::Virtual, json, error)) << error;
    ed::Document loaded;
    ASSERT_TRUE (geo::ReadLandscape (json, loaded, error)) << error;
    EXPECT_EQ (loaded.entities, after.entities);
    EXPECT_EQ (loaded.landscape, after.landscape);
    // One undo restores both lines and both live sources; redo is the same result.
    EXPECT_EQ (session.UndoDepth (), 1u);
    ASSERT_TRUE (session.Undo ());
    EXPECT_EQ (session.Doc ().entities, doc.entities);
    EXPECT_EQ (session.Doc ().landscape->sources, doc.landscape->sources);
    ASSERT_TRUE (session.Redo ());
    EXPECT_EQ (session.Doc ().entities, after.entities);
}

TEST (EditBoolean, FurtherPiecesBecomeReferenceLinesOfTheKeptStyle)
{
    ReferenceScene scene (20, 0, 24, 4); // apart: a union of two pieces
    geo::ReferenceLineStyle style;
    style.rgba = 0xCC3300FF;
    ASSERT_TRUE (geo::SetReferenceLineStyle (scene.doc, LiveSource (scene.doc, scene.a)->id, style, scene.error))
        << scene.error;
    std::vector<ed::Id> made;
    ASSERT_TRUE (Apply (scene.doc, { scene.a, scene.b }, ed::BooleanKind::Union, made, scene.error)) << scene.error;
    ASSERT_EQ (made.size (), 2u);
    EXPECT_EQ (made[0], scene.a);
    for (const ed::Id id : made) {
        const auto* source = LiveSource (scene.doc, id);
        ASSERT_NE (source, nullptr) << "every piece stays a reference line";
        EXPECT_EQ (source->role, geo::SourceRole::ReferenceLine);
        EXPECT_EQ (source->lineStyle, style);
    }
    EXPECT_EQ (LiveSource (scene.doc, scene.b), nullptr);
    EXPECT_TRUE (ed::Validate (scene.doc, scene.error)) << scene.error;
}

TEST (EditBoolean, ProtectedLandscapeOperandsRefuseWithTheirReason)
{
    const auto refused = [] (ReferenceScene& scene, const char* reason) {
        const auto entities = scene.doc.entities;
        const auto landscape = scene.doc.landscape;
        const ed::Id nextId = scene.doc.nextId;
        std::vector<ed::Id> made;
        std::string error;
        EXPECT_FALSE (Apply (scene.doc, { scene.a, scene.b }, ed::BooleanKind::Union, made, error));
        EXPECT_NE (error.find (reason), std::string::npos) << error;
        EXPECT_EQ (ed::BooleanOwnershipRefusal (scene.doc, scene.b), error);
        EXPECT_EQ (scene.doc.entities, entities);
        EXPECT_EQ (scene.doc.landscape, landscape);
        EXPECT_EQ (scene.doc.nextId, nextId);
    };
    {
        ReferenceScene scene (2, 2, 6, 6);
        geo::ReferenceLineStyle locked;
        locked.locked = true;
        ASSERT_TRUE (geo::SetReferenceLineStyle (scene.doc, LiveSource (scene.doc, scene.b)->id, locked, scene.error));
        refused (scene, "locked");
    }
    for (const auto role : { geo::SourceRole::RegionBoundary, geo::SourceRole::Guide }) {
        ReferenceScene scene (2, 2, 6, 6);
        ASSERT_TRUE (geo::SetLandscapeSourceRole (scene.doc, scene.b, role, scene.error)) << scene.error;
        refused (scene, "landscape panel");
    }
    {
        // Archicad's own line, imported by reference.
        ed::Document doc;
        std::string error;
        ASSERT_TRUE (geo::StartLandscape (doc, kSession, error));
        const ed::Id a = Box (doc, 0, 0, 10, 10);
        geo::LandscapeSource imported;
        imported.entityId = Box (doc, 2, 2, 6, 6);
        imported.reference = true;
        imported.archicadGuid = "00112233-4455-6677-8899-AABBCCDDEEFF";
        ed::Id id = ed::kNoId;
        ASSERT_TRUE (geo::AddLandscapeSource (doc, imported, id, error)) << error;
        const auto before = doc.entities;
        std::vector<ed::Id> made;
        EXPECT_FALSE (Apply (doc, { a, imported.entityId }, ed::BooleanKind::Union, made, error));
        EXPECT_NE (error.find ("Archicad"), std::string::npos) << error;
        EXPECT_EQ (doc.entities, before);
    }
}

TEST (EditBoolean, ACompoundReferenceLineCannotSilentlyBecomeARegion)
{
    ReferenceScene scene (3, 3, 7, 7);
    std::vector<ed::Id> made;
    ASSERT_TRUE (Apply (scene.doc, { scene.a, scene.b }, ed::BooleanKind::Subtract, made, scene.error)) << scene.error;
    const auto before = scene.doc.landscape;
    EXPECT_FALSE (geo::SetLandscapeSourceRole (scene.doc, scene.a, geo::SourceRole::RegionBoundary, scene.error));
    EXPECT_NE (scene.error.find ("holes"), std::string::npos) << scene.error;
    EXPECT_EQ (scene.doc.landscape, before);
}
