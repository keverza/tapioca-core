// Compound closed entities: one entity, several contours, even-odd fill (EditDocument.hpp).
#include "ArchViz/EditLayers.hpp"
#include "Edit/EditArrange.hpp"
#include "Edit/EditCompound.hpp"
#include "Edit/EditGizmo.hpp"
#include "Edit/EditImport.hpp"
#include "Edit/EditLandscape.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditPolyline.hpp"
#include "Edit/EditSelection.hpp"
#include "Edit/EditSession.hpp"
#include "Edit/EditShape.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>

namespace ed = geomsrv::edit;
namespace el = geomsrv::archviz::editlayers;
namespace geo = tapioca::geometry;

namespace {

std::vector<ed::Vec3> Square (double x0, double y0, double x1, double y1, double z = 0.0)
{
    return { { x0, y0, z }, { x1, y0, z }, { x1, y1, z }, { x0, y1, z } };
}

// A 10 x 10 outline with a 4 x 4 hole: 84 m2 of solid.
ed::Id Donut (ed::Document& doc, ed::Kind kind = ed::Kind::Polyline, double height = 0.0)
{
    return ed::AddCompound (doc, { Square (0, 0, 10, 10), Square (3, 3, 7, 7) }, kind, height);
}

double RingArea (const std::vector<ed::Vec3>& ring)
{
    double twice = 0.0;
    for (size_t i = 0; i < ring.size (); ++i) {
        const auto a = ring[i], b = ring[(i + 1) % ring.size ()];
        twice += a.x * b.y - b.x * a.y;
    }
    return twice * 0.5;
}

ed::PickView PlanView ()
{
    ed::PickView view;
    view.toScreen = [] (ed::Vec3 p, double& x, double& y) {
        x = p.x * 10.0;
        y = p.y * 10.0;
        return true;
    };
    return view;
}

bool InsideHit (const std::vector<ed::PickHit>& hits, ed::Id entity)
{
    for (const auto& h : hits)
        if (h.ref.entity == entity && (h.ref.part == ed::Part::Entity || h.ref.part == ed::Part::Face))
            return true;
    return false;
}

std::string Write (const ed::Document& doc)
{
    std::string text, error;
    EXPECT_TRUE (geo::WriteLandscape (doc, geo::LandscapeStorage::Virtual, text, error)) << error;
    return text;
}

ed::Document WithLandscape ()
{
    ed::Document doc;
    std::string error;
    EXPECT_TRUE (geo::StartLandscape (doc, { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, error));
    return doc;
}

} // namespace

TEST (EditCompound, RingsAreWoundByDepthAndAreaIsEvenOdd)
{
    ed::Document doc;
    // Both rings given clockwise: the outer is turned counter-clockwise, the hole stays clockwise.
    auto outer = Square (0, 0, 10, 10), hole = Square (3, 3, 7, 7);
    std::reverse (outer.begin (), outer.end ());
    std::reverse (hole.begin (), hole.end ());
    const ed::Id id = ed::AddCompound (doc, { outer, hole }, ed::Kind::Polyline, 0.0);
    ASSERT_NE (id, ed::kNoId);
    const ed::Entity& e = *ed::Find (doc, id);
    std::string error;
    ASSERT_TRUE (ed::Validate (doc, error)) << error;
    EXPECT_TRUE (ed::IsCompound (e));
    EXPECT_TRUE (e.closed);
    ASSERT_EQ (ed::ContourCount (e), 2u);
    EXPECT_NE (e.contours[0].id, e.contours[1].id);
    EXPECT_GT (RingArea (ed::ContourPoints (e, 0)), 0.0);
    EXPECT_LT (RingArea (ed::ContourPoints (e, 1)), 0.0);
    EXPECT_NEAR (ed::SignedAreaXY (e), 84.0, 1e-9);
    EXPECT_EQ (ed::ContourDepth (e, 0), 0u);
    EXPECT_EQ (ed::ContourDepth (e, 1), 1u);
    EXPECT_TRUE (ed::InsideXY (e, { 1, 1, 0 }));
    EXPECT_FALSE (ed::InsideXY (e, { 5, 5, 0 }));
    EXPECT_FALSE (ed::InsideXY (e, { 11, 5, 0 }));
    // Ids come from the document's one counter: every vertex, segment and contour is distinct.
    EXPECT_EQ (e.segments.size (), 8u);
    EXPECT_GT (doc.nextId, e.contours[1].id);
}

TEST (EditCompound, NestedIslandIsSolidAgain)
{
    ed::Document doc;
    const ed::Id id = ed::AddCompound (doc, { Square (0, 0, 10, 10), Square (2, 2, 8, 8), Square (4, 4, 6, 6) },
                                       ed::Kind::Polyline, 0.0);
    ASSERT_NE (id, ed::kNoId);
    const ed::Entity& e = *ed::Find (doc, id);
    EXPECT_EQ (ed::ContourDepth (e, 2), 2u);
    EXPECT_GT (RingArea (ed::ContourPoints (e, 2)), 0.0); // an island winds like the outer
    EXPECT_NEAR (ed::SignedAreaXY (e), 100.0 - 36.0 + 4.0, 1e-9);
    EXPECT_TRUE (ed::InsideXY (e, { 5, 5, 0 }));
    EXPECT_FALSE (ed::InsideXY (e, { 3, 5, 0 }));
}

TEST (EditCompound, NeighboursWrapWithinTheirContour)
{
    ed::Document doc;
    const ed::Entity& e = *ed::Find (doc, Donut (doc));
    size_t begin = 0, end = 0;
    ed::ContourRange (e, 1, begin, end);
    EXPECT_EQ (begin, 4u);
    EXPECT_EQ (end, 8u);
    EXPECT_EQ (ed::NextVertex (e, 3), 0u); // the outer's last back to the outer's first
    EXPECT_EQ (ed::NextVertex (e, 7), 4u); // the hole's last back to the hole's first
    EXPECT_EQ (ed::PrevVertex (e, 4), 7u);
    size_t from = 0, to = 0;
    ed::SegmentEnds (e, 7, from, to);
    EXPECT_EQ (from, 7u);
    EXPECT_EQ (to, 4u);
    EXPECT_EQ (ed::ContourOf (e, 5), 1u);
    EXPECT_EQ (ed::ContourIndex (e, e.contours[1].id), 1);
    EXPECT_EQ (ed::ContourIndex (e, e.id), -1);
    // The drawn outline and the snap features never bridge two contours.
    const auto rings = ed::ShapeRings (e);
    ASSERT_EQ (rings.size (), 2u);
    EXPECT_EQ (rings[1].size (), 4u);
    for (const auto& edge : ed::Features (e).edges)
        EXPECT_LT (ed::DistanceXY (edge.a, edge.b), 10.0 + 1e-9);
}

TEST (EditCompound, MalformedParentageIsRefused)
{
    ed::Document doc;
    // A ring outside the outer, a ring crossing it, a ring touching another.
    EXPECT_EQ (ed::AddCompound (doc, { Square (0, 0, 10, 10), Square (20, 20, 22, 22) }, ed::Kind::Polyline, 0),
               ed::kNoId);
    EXPECT_EQ (ed::AddCompound (doc, { Square (0, 0, 10, 10), Square (8, 8, 12, 12) }, ed::Kind::Polyline, 0),
               ed::kNoId);
    EXPECT_EQ (ed::AddCompound (doc, { Square (0, 0, 10, 10), Square (2, 2, 4, 4), Square (4, 2, 6, 4) },
                                ed::Kind::Polyline, 0),
               ed::kNoId);
    EXPECT_TRUE (doc.entities.empty ());

    const ed::Id id = Donut (doc);
    std::string error;
    auto broken = doc;
    ed::Find (broken, id)->contours[1].count = 5; // counts no longer add up
    EXPECT_FALSE (ed::Validate (broken, error));
    broken = doc;
    ed::Find (broken, id)->contours.pop_back (); // a table of one
    EXPECT_FALSE (ed::Validate (broken, error));
    broken = doc;
    ed::Find (broken, id)->contours[1].id = ed::Find (broken, id)->vertices[0].id; // shared id
    EXPECT_FALSE (ed::Validate (broken, error));
    EXPECT_NE (error.find ("duplicate"), std::string::npos);
    broken = doc;
    auto& hole = ed::Find (broken, id)->vertices;
    std::reverse (hole.begin () + 4, hole.end ()); // the hole wound counter-clockwise
    EXPECT_FALSE (ed::Validate (broken, error));
    EXPECT_NE (error.find ("clockwise"), std::string::npos);
    broken = doc;
    ed::Find (broken, id)->vertices[5].p = { 12, 3, 0 }; // a hole vertex dragged outside
    EXPECT_FALSE (ed::Validate (broken, error));
}

TEST (EditCompound, CompoundExtrusionFacesKeepIndicesAndPointOutOfTheSolid)
{
    ed::Document doc;
    const ed::Id id = Donut (doc, ed::Kind::Extrusion, 2.0);
    ASSERT_NE (id, ed::kNoId);
    const ed::Entity& e = *ed::Find (doc, id);
    std::string error;
    ASSERT_TRUE (ed::Validate (doc, error)) << error;
    EXPECT_EQ (ed::FaceCount (e), 2u + 8u);
    // Face 2 + i stands on segment i: the hole is stored clockwise from (3,7), so face 6 is its
    // top wall, (3,7) to (7,7); its outside (the hole) is -Y.
    ed::Vec3 n;
    ASSERT_TRUE (ed::FaceNormal (e, 6, n));
    EXPECT_NEAR (n.y, -1.0, 1e-12);
    ASSERT_TRUE (ed::FaceNormal (e, 2, n)); // the outer's first wall faces -Y
    EXPECT_NEAR (n.y, -1.0, 1e-12);
    const auto top = ed::FaceRings (e, 1);
    ASSERT_EQ (top.size (), 2u);
    EXPECT_NEAR (RingArea (top[0]) + RingArea (top[1]), 84.0, 1e-9);
    EXPECT_NEAR (top[1][0].z, 2.0, 1e-12);
    const auto bottom = ed::FaceRings (e, 0); // seen from below: every ring runs backwards
    EXPECT_NEAR (RingArea (bottom[0]) + RingArea (bottom[1]), -84.0, 1e-9);
    EXPECT_EQ (ed::FacePolygon (e, 1).size (), 4u); // the outer only

    // A simple box keeps the indexing it always had.
    const ed::Id box = ed::AddExtrusion (doc, Square (20, 0, 24, 3), 3.0);
    EXPECT_EQ (ed::FaceRings (*ed::Find (doc, box), 1).size (), 1u);
    EXPECT_EQ (ed::FaceCount (*ed::Find (doc, box)), 6u);
}

TEST (EditCompound, PushingAHoleWallWidensTheHole)
{
    ed::Document doc;
    const ed::Id id = Donut (doc, ed::Kind::Extrusion, 2.0);
    ed::Transform t;
    t.extrude = 1.0;
    std::string error;
    ASSERT_TRUE (ed::TransformOperation ({ { id, ed::Part::Face, 6 } }, t) (doc, error)) << error;
    const ed::Entity& e = *ed::Find (doc, id);
    EXPECT_NEAR (e.vertices[4].p.y, 6.0, 1e-12); // the wall at y = 7 moved into the hole
    EXPECT_NEAR (e.vertices[5].p.y, 6.0, 1e-12);
    EXPECT_NEAR (ed::SignedAreaXY (e), 100.0 - 12.0, 1e-9);
    ASSERT_TRUE (ed::Validate (doc, error)) << error;
}

TEST (EditCompound, PickingAHoleIsNotPickingTheSolid)
{
    ed::Document doc;
    const ed::Id ring = Donut (doc);
    const ed::Id solid =
        ed::AddCompound (doc, { Square (20, 0, 30, 10), Square (23, 3, 27, 7) }, ed::Kind::Extrusion, 2.0);
    const auto view = PlanView ();
    EXPECT_TRUE (InsideHit (ed::Pick (doc, view, 10, 10, ed::kPickAll, 3.0), ring));   // (1,1): solid
    EXPECT_FALSE (InsideHit (ed::Pick (doc, view, 50, 50, ed::kPickAll, 3.0), ring));  // (5,5): hole
    EXPECT_TRUE (InsideHit (ed::Pick (doc, view, 210, 10, ed::kPickAll, 3.0), solid)); // (21,1)
    EXPECT_FALSE (InsideHit (ed::Pick (doc, view, 250, 50, ed::kPickAll, 3.0), solid));
    // The hole's own edge is a segment of the same entity.
    bool holeEdge = false;
    for (const auto& h : ed::Pick (doc, view, 30, 50, ed::kPickSegment, 3.0)) // (3,5)
        holeEdge = holeEdge || (h.ref.entity == ring && h.ref.part == ed::Part::Segment &&
                                ed::SegmentIndex (*ed::Find (doc, ring), h.ref.item) >= 4);
    EXPECT_TRUE (holeEdge);
}

TEST (EditCompound, VertexEditsStayInTheirContourAndRefuseBelowThree)
{
    ed::Document doc;
    const ed::Id id = Donut (doc);
    ed::Session session (doc);
    const ed::Entity before = *ed::Find (session.Doc (), id);
    const ed::Id holeClosing = before.segments[7].id; // (3,7) back to (3,3)
    ASSERT_TRUE (session.Apply ([&] (ed::Document& d, std::string& error) {
        return ed::InsertVertex (d, id, holeClosing, { 3, 5, 0 }, error);
    })) << session.Message ();
    const ed::Entity& grown = *ed::Find (session.Doc (), id);
    EXPECT_EQ (grown.contours[0].count, 4u);
    EXPECT_EQ (grown.contours[1].count, 5u);
    EXPECT_EQ (grown.contours,
               std::vector<ed::EntityContour> ({ { before.contours[0].id, 4 }, { before.contours[1].id, 5 } }));
    EXPECT_NEAR (grown.vertices[8].p.y, 5.0, 1e-12); // appended at the hole's end
    ASSERT_TRUE (session.Undo ());
    EXPECT_EQ (*ed::Find (session.Doc (), id), before); // one undo

    // A hole of three cannot lose a vertex; the outer can.
    ed::Document tri;
    const ed::Id t = ed::AddCompound (tri, { Square (0, 0, 10, 10), { { 4, 4, 0 }, { 6, 4, 0 }, { 5, 6, 0 } } },
                                      ed::Kind::Polyline, 0);
    std::string error;
    auto attempt = tri;
    EXPECT_FALSE (ed::DeleteVertex (attempt, t, ed::Find (tri, t)->vertices[5].id, error));
    EXPECT_NE (error.find ("hole"), std::string::npos);
    ed::Document five;
    const ed::Id f = ed::AddCompound (
        five, { { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 5, 12, 0 }, { 0, 10, 0 } }, Square (3, 3, 7, 7) },
        ed::Kind::Polyline, 0);
    ASSERT_TRUE (ed::DeleteVertex (five, f, ed::Find (five, f)->vertices[3].id, error)) << error;
    EXPECT_EQ (ed::Find (five, f)->contours[0].count, 4u);
    EXPECT_TRUE (ed::Validate (five, error)) << error;
}

TEST (EditCompound, AmbiguousTopologyEditsRefuseAtomically)
{
    ed::Document doc;
    const ed::Id id = Donut (doc);
    const ed::Document before = doc;
    std::string error;
    const ed::Id vertex = ed::Find (doc, id)->vertices[0].id;
    EXPECT_FALSE (ed::ReverseDirection (doc, id, error));
    EXPECT_FALSE (ed::Open (doc, id, error));
    EXPECT_FALSE (ed::SplitAtVertex (doc, id, vertex, error));
    EXPECT_FALSE (ed::OffsetEntity (doc, id, 0.5, error));
    EXPECT_FALSE (ed::TrimBetweenCrossings (doc, id, ed::Find (doc, id)->segments[0].id, { 5, 0, 0 }, error));
    EXPECT_EQ (doc.entities, before.entities);

    // Through a session a refused edit leaves no undo entry and nothing changed.
    ed::Session session (doc);
    EXPECT_FALSE (session.Apply ([&] (ed::Document& d, std::string& e) { return ed::Open (d, id, e); }));
    EXPECT_EQ (session.UndoDepth (), 0u);
    // An edit that would push a hole through the outer is refused by validation.
    EXPECT_FALSE (session.Apply ([&] (ed::Document& d, std::string& e) {
        const auto* h = ed::Find (d, id);
        return ed::MoveVertex (d, id, h->vertices[6].id, { 11, 8, 0 }, e);
    }));
    EXPECT_EQ (session.Doc ().entities, before.entities);
}

TEST (EditCompound, CornersRoundWithinTheirContour)
{
    ed::Document doc;
    const ed::Id id = Donut (doc);
    std::string error;
    const ed::Id holeCorner = ed::Find (doc, id)->vertices[4].id;
    ASSERT_TRUE (ed::SetCornerRadius (doc, id, { holeCorner }, 0.5, error)) << error;
    ASSERT_TRUE (ed::Validate (doc, error)) << error;
    const auto rings = ed::ShapeRings (*ed::Find (doc, id));
    EXPECT_GT (rings[1].size (), 4u);
    EXPECT_EQ (rings[0].size (), 4u);
    // A baked fillet inserts points into the hole only.
    ASSERT_TRUE (ed::Fillet (doc, id, ed::Find (doc, id)->vertices[6].id, 0.5, 4, error)) << error;
    EXPECT_EQ (ed::Find (doc, id)->contours[0].count, 4u);
    EXPECT_GT (ed::Find (doc, id)->contours[1].count, 4u);
    EXPECT_TRUE (ed::Validate (doc, error)) << error;
}

TEST (EditCompound, MirrorAndPasteKeepAValidCompound)
{
    ed::Document doc;
    const ed::Id id = Donut (doc, ed::Kind::Extrusion, 1.0);
    std::string error;
    ASSERT_TRUE (ed::Mirror (doc, { { id, ed::Part::Entity, 0 } }, { 0, 0, 0 }, { 0, 1, 0 }, error)) << error;
    ASSERT_TRUE (ed::Validate (doc, error)) << error;
    EXPECT_NEAR (ed::SignedAreaXY (*ed::Find (doc, id)), 84.0, 1e-9);

    const auto clip = ed::Copy (doc, { { id, ed::Part::Entity, 0 } });
    std::vector<ed::Ref> pasted;
    ASSERT_TRUE (ed::Paste (doc, clip, { 30, 0, 0 }, pasted, error)) << error;
    ASSERT_EQ (pasted.size (), 1u);
    const auto& copy = *ed::Find (doc, pasted[0].entity);
    EXPECT_EQ (copy.contours.size (), 2u);
    EXPECT_NE (copy.contours[0].id, ed::Find (doc, id)->contours[0].id);
    EXPECT_NE (copy.contours[1].id, ed::Find (doc, id)->contours[1].id);
    ASSERT_TRUE (ed::Validate (doc, error)) << error;
}

TEST (EditCompound, SetRingsKeepsTheEntityAndCanMakeItSimple)
{
    ed::Document doc;
    const ed::Id id = ed::AddPolyline (doc, Square (0, 0, 10, 10), true, "platform");
    std::string error;
    ASSERT_TRUE (ed::SetRings (doc, id, { Square (0, 0, 10, 10), Square (2, 2, 4, 4) }, error)) << error;
    EXPECT_EQ (ed::Find (doc, id)->role, "platform");
    EXPECT_TRUE (ed::IsCompound (*ed::Find (doc, id)));
    ASSERT_TRUE (ed::Validate (doc, error)) << error;
    const auto before = doc;
    EXPECT_FALSE (ed::SetRings (doc, id, { Square (0, 0, 10, 10), Square (8, 8, 12, 12) }, error));
    EXPECT_EQ (doc.entities, before.entities);
    ASSERT_TRUE (ed::SetRings (doc, id, { Square (0, 0, 5, 5) }, error)) << error;
    EXPECT_FALSE (ed::IsCompound (*ed::Find (doc, id)));
    EXPECT_TRUE (ed::Find (doc, id)->contours.empty ());
    EXPECT_TRUE (ed::Validate (doc, error)) << error;
}

TEST (EditCompound, PersistenceRoundTripsAndOldDocumentsStayOld)
{
    auto doc = WithLandscape ();
    ed::AddPolyline (doc, Square (40, 0, 44, 4), true);
    std::string error;
    const std::string simple = Write (doc);
    EXPECT_NE (simple.find ("\"version\":2"), std::string::npos); // no hole, no new schema
    EXPECT_EQ (simple.find ("contours"), std::string::npos);

    const ed::Id id = Donut (doc, ed::Kind::Extrusion, 1.5);
    ASSERT_NE (id, ed::kNoId);
    const std::string text = Write (doc);
    EXPECT_NE (text.find ("\"version\":3"), std::string::npos);
    ed::Document loaded;
    ASSERT_TRUE (geo::ReadLandscape (text, loaded, error)) << error;
    EXPECT_EQ (loaded.entities, doc.entities);
    EXPECT_EQ (loaded.nextId, doc.nextId);
    ASSERT_TRUE (ed::Validate (loaded, error)) << error;

    // A version-2 reader never accepts contours; a malformed table is refused, not repaired.
    auto v2 = text;
    v2.replace (v2.find ("\"version\":3"), 11, "\"version\":2");
    EXPECT_FALSE (geo::ReadLandscape (v2, loaded, error));
    auto bad = text;
    const auto at = bad.find ("\"count\":\"4\"");
    ASSERT_NE (at, std::string::npos);
    bad.replace (at, 11, "\"count\":\"2\"");
    EXPECT_FALSE (geo::ReadLandscape (bad, loaded, error));
    bad = text;
    bad.replace (bad.find ("\"count\":\"4\""), 11, "\"count\":\"5\"");
    EXPECT_FALSE (geo::ReadLandscape (bad, loaded, error)); // counts no longer add up
}

TEST (EditCompound, SolidCapsKeepTheirHolesAndHoleWallsAreDrawn)
{
    ed::Document doc;
    Donut (doc, ed::Kind::Extrusion, 2.0);
    el::Feedback f;
    const auto layer = el::Build (doc, f);
    ASSERT_EQ (layer.meshes.size (), 1u);
    const auto& mesh = layer.meshes.front ();
    double up = 0.0, down = 0.0;
    size_t walls = 0;
    for (size_t t = 0; t + 2 < mesh.indices.size (); t += 3) {
        const auto P = [&] (uint32_t i) {
            return ed::Vec3 { mesh.points[3 * i], mesh.points[3 * i + 1], mesh.points[3 * i + 2] };
        };
        const auto a = P (mesh.indices[t]), b = P (mesh.indices[t + 1]), c = P (mesh.indices[t + 2]);
        const double area = 0.5 * ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
        if (std::abs (area) < 1e-12)
            ++walls;
        else if (area > 0)
            up += area;
        else
            down -= area;
    }
    EXPECT_NEAR (up, 84.0, 1e-6); // the top, with its hole
    EXPECT_NEAR (down, 84.0, 1e-6);
    EXPECT_EQ (walls, 2u * 8u); // two triangles up every one of the eight walls
    // Every contour is outlined at the base and the top.
    EXPECT_GE (layer.polylines.size (), 4u);
}

TEST (EditCompound, ImportedFillContoursStayOneEntity)
{
    ed::Document doc;
    // A fill as the host gives it: the hole first, both closed with the first point repeated,
    // both counter-clockwise, and an arc on the outline.
    ed::ImportedCurve hole { { { 3, 3, 0 }, { 7, 3, 0 }, { 7, 7, 0 }, { 3, 7, 0 }, { 3, 3, 0 } }, {}, true };
    ed::ImportedCurve outline { { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 }, { 0, 0, 0 } },
                                { 0, 2.0, 0, 0, 0 }, // over a quarter turn: split in two
                                true };
    std::string error;
    const ed::Id id = ed::AddImportedFill (doc, { hole, outline }, "imported", error);
    ASSERT_NE (id, ed::kNoId) << error;
    ASSERT_EQ (doc.entities.size (), 1u);
    const ed::Entity& e = *ed::Find (doc, id);
    ASSERT_EQ (e.contours.size (), 2u);
    EXPECT_GT (e.contours[0].count, 4u); // the outline, with its arc split into cubic pieces, first
    EXPECT_EQ (e.contours[1].count, 4u);
    EXPECT_FALSE (ed::InsideXY (e, { 5, 5, 0 }));
    EXPECT_TRUE (ed::InsideXY (e, { 1, 1, 0 }));
    EXPECT_TRUE (ed::Validate (doc, error)) << error;

    // One contour is an ordinary outline; contours that do not nest are refused whole.
    EXPECT_TRUE (ed::Find (doc, ed::AddImportedFill (doc, { outline }, "imported", error))->contours.empty ());
    const size_t count = doc.entities.size ();
    ed::ImportedCurve away { { { 20, 0, 0 }, { 22, 0, 0 }, { 22, 2, 0 } }, {}, true };
    EXPECT_EQ (ed::AddImportedFill (doc, { outline, away }, "imported", error), ed::kNoId);
    EXPECT_EQ (doc.entities.size (), count);
}

TEST (EditCompound, LandscapeSourcesStaySimple)
{
    auto doc = WithLandscape ();
    const ed::Id id = Donut (doc);
    std::string error;
    EXPECT_FALSE (geo::SetLandscapeSourceRole (doc, id, geo::SourceRole::Guide, error) && ed::Validate (doc, error));
    EXPECT_NE (error.find ("holes"), std::string::npos);
}

TEST (EditCompound, CapsCoverEvenOddWithIslandsAndWallsFaceOut)
{
    ed::Document doc;
    const ed::Id id = ed::AddCompound (doc, { Square (0, 0, 10, 10), Square (2, 2, 8, 8), Square (4, 4, 6, 6) },
                                       ed::Kind::Extrusion, 1.0);
    ASSERT_NE (id, ed::kNoId);
    std::vector<ed::Vec3> points;
    std::vector<uint32_t> triangles;
    std::string error;
    ASSERT_TRUE (ed::CapTriangles (*ed::Find (doc, id), points, triangles, error)) << error;
    double area = 0.0;
    for (size_t t = 0; t + 2 < triangles.size (); t += 3) {
        const auto a = points[triangles[t]], b = points[triangles[t + 1]], c = points[triangles[t + 2]];
        const double twice = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        EXPECT_GT (twice, 0.0); // counter-clockwise from above
        area += twice * 0.5;
        // No triangle lies in the hole ring between the island and the hole's edge.
        const ed::Vec3 centre { (a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, 0 };
        EXPECT_TRUE (ed::InsideXY (*ed::Find (doc, id), centre));
    }
    EXPECT_NEAR (area, 100.0 - 36.0 + 4.0, 1e-9);

    ASSERT_TRUE (ed::SolidTriangles (*ed::Find (doc, id), points, triangles, error)) << error;
    // Each wall's outward normal points away from the solid: a step along it leaves the profile.
    for (size_t t = 0; t + 2 < triangles.size (); t += 3) {
        const auto a = points[triangles[t]], b = points[triangles[t + 1]], c = points[triangles[t + 2]];
        const ed::Vec3 u = b - a, v = c - a;
        const ed::Vec3 n { u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
        if (std::abs (n.z) > 1e-12)
            continue; // a cap
        const double length = std::hypot (n.x, n.y);
        const ed::Vec3 mid { (a.x + b.x + c.x) / 3 + n.x / length * 0.1, (a.y + b.y + c.y) / 3 + n.y / length * 0.1,
                             0 };
        EXPECT_FALSE (ed::InsideXY (*ed::Find (doc, id), mid));
    }
}

TEST (EditCompound, DeletingAHoleKeepsTheOuterAndTakesItsIslands)
{
    ed::Document doc;
    const ed::Id id = ed::AddCompound (
        doc, { Square (0, 0, 20, 10), Square (2, 2, 8, 8), Square (4, 4, 6, 6), Square (12, 2, 18, 8) },
        ed::Kind::Polyline, 0);
    ASSERT_NE (id, ed::kNoId);
    const ed::Entity before = *ed::Find (doc, id);
    std::string error;
    // The outer contour is the object: it is never removed on its own.
    auto attempt = doc;
    EXPECT_FALSE (ed::DeleteContour (attempt, id, before.contours[0].id, error));
    EXPECT_EQ (attempt.entities, doc.entities);

    // The first hole goes with the island inside it; the other hole stays, ids intact.
    ASSERT_TRUE (ed::DeleteContour (doc, id, before.contours[1].id, error)) << error;
    const ed::Entity& one = *ed::Find (doc, id);
    ASSERT_EQ (one.contours.size (), 2u);
    EXPECT_EQ (one.contours[0], before.contours[0]);
    EXPECT_EQ (one.contours[1], before.contours[3]);
    EXPECT_NEAR (ed::SignedAreaXY (one), 200.0 - 36.0, 1e-9);
    ASSERT_TRUE (ed::Validate (doc, error)) << error;

    // The last hole: a simple entity again, with the outer's vertices and segments unchanged.
    ASSERT_TRUE (ed::DeleteContour (doc, id, before.contours[3].id, error)) << error;
    const ed::Entity& simple = *ed::Find (doc, id);
    EXPECT_TRUE (simple.contours.empty ());
    EXPECT_EQ (simple.vertices, std::vector<ed::Vertex> (before.vertices.begin (), before.vertices.begin () + 4));
    EXPECT_TRUE (ed::Validate (doc, error)) << error;
}

TEST (EditCompound, DeleteHoleActionIsOfferedOnHolePartsOnly)
{
    ed::Document doc;
    const ed::Id id = Donut (doc);
    const ed::Entity& e = *ed::Find (doc, id);
    const auto table = ed::PolylineActions ();
    const ed::Action* action = nullptr;
    for (const auto& a : table)
        if (a.id == "delete-hole")
            action = &a;
    ASSERT_NE (action, nullptr);
    EXPECT_FALSE (action->make (doc, { { id, ed::Part::Vertex, e.vertices[0].id } })); // the outer
    EXPECT_FALSE (action->make (doc, { { id, ed::Part::Segment, e.segments[2].id } }));
    const auto remove =
        action->make (doc, { { id, ed::Part::Segment, e.segments[5].id }, { id, ed::Part::Vertex, e.vertices[6].id } });
    ASSERT_TRUE (remove);
    ed::Session session (doc);
    ASSERT_TRUE (session.Apply (remove)) << session.Message ();
    EXPECT_FALSE (ed::IsCompound (*ed::Find (session.Doc (), id)));
    EXPECT_EQ (session.UndoDepth (), 1u);
    ASSERT_TRUE (session.Undo ());
    EXPECT_TRUE (ed::IsCompound (*ed::Find (session.Doc (), id)));
}
