#include "Edit/EditArrange.hpp"
#include "Edit/EditEllipse.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPolyline.hpp"
#include "Edit/EditPrimitives.hpp"
#include "Edit/EditSession.hpp"
#include <gtest/gtest.h>
#include <set>

namespace ed = geomsrv::edit;

namespace {
void Valid (const ed::Document& doc)
{
    std::string why;
    EXPECT_TRUE (ed::Validate (doc, why)) << why;
}

std::set<ed::Id> AllIds (const ed::Document& doc)
{
    std::set<ed::Id> ids;
    for (const auto& e : doc.entities) {
        ids.insert (e.id);
        for (const auto& v : e.vertices)
            ids.insert (v.id);
        for (const auto& s : e.segments)
            ids.insert (s.id);
    }
    for (const auto& p : doc.paths) {
        ids.insert (p.id);
        for (const auto& c : p.points)
            ids.insert (c.id);
        for (const auto& b : p.profile)
            ids.insert (b.id);
        for (const auto& s : p.sections)
            ids.insert (s.id);
    }
    return ids;
}
} // namespace

TEST (EditArrange, CopyTakesEachWholeObjectOnceWhateverPartWasPicked)
{
    ed::Document doc;
    const ed::Id a = ed::AddPolyline (doc, { { 0, 0, 0 }, { 2, 0, 0 }, { 2, 2, 0 } }, false);
    const ed::Id b = ed::AddPolyline (doc, { { 5, 0, 0 }, { 6, 0, 0 } }, false);
    const ed::Id p = ed::AddPath (doc, { { 0, 5, 0 }, { 10, 5, 0 } }, 1.5);
    const auto& first = *ed::Find (doc, a);
    const auto clip = ed::Copy (doc, { { a, ed::Part::Vertex, first.vertices[1].id },
                                       { a, ed::Part::Segment, first.segments[0].id },
                                       { p, ed::Part::PathPoint, ed::FindPath (doc, p)->points[0].id } });
    ASSERT_EQ (clip.entities.size (), 1u);
    EXPECT_EQ (clip.entities[0], first);
    ASSERT_EQ (clip.paths.size (), 1u);
    EXPECT_TRUE (ed::Copy (doc, { { b, ed::Part::LandscapeFace, 0 } }).Empty ());
    ed::Vec3 centre;
    ASSERT_TRUE (ed::Centre (clip, centre));
    EXPECT_DOUBLE_EQ (centre.x, 5.0);
    EXPECT_DOUBLE_EQ (centre.y, 2.5);
}

TEST (EditArrange, PasteAddsMovedCopiesWithFreshIdsAndTheirAttributes)
{
    ed::Document doc;
    const ed::Id a = ed::AddPolyline (doc, { { 0, 0, 1 }, { 2, 0, 1 }, { 2, 2, 1 } }, true, "platform");
    ed::Find (doc, a)->segments[1].width = 0.3;
    ed::Find (doc, a)->vertices[2].radius = 0.25;
    const auto before = AllIds (doc);
    const auto clip = ed::Copy (doc, { { a, ed::Part::Entity, 0 } });
    std::vector<ed::Ref> pasted;
    std::string error;
    ASSERT_TRUE (ed::Paste (doc, clip, { 10, 0, 0 }, pasted, error)) << error;
    ASSERT_TRUE (ed::Paste (doc, clip, { 20, 0, 0 }, pasted, error)) << error;
    ASSERT_EQ (doc.entities.size (), 3u);
    ASSERT_EQ (pasted.size (), 1u);
    EXPECT_EQ (pasted[0].entity, doc.entities[2].id);
    const auto& copy = doc.entities[1];
    EXPECT_EQ (copy.role, "platform");
    EXPECT_TRUE (copy.closed);
    EXPECT_DOUBLE_EQ (copy.segments[1].width, 0.3);
    EXPECT_DOUBLE_EQ (copy.vertices[2].radius, 0.25);
    EXPECT_DOUBLE_EQ (copy.vertices[0].p.x, 10.0);
    EXPECT_DOUBLE_EQ (copy.vertices[0].p.z, 1.0);
    // Every id is new, and none repeats across the two pastes.
    const auto after = AllIds (doc);
    EXPECT_EQ (after.size (), before.size () * 3);
    Valid (doc);
    EXPECT_FALSE (ed::Paste (doc, {}, {}, pasted, error));
}

TEST (EditArrange, PasteAndMirrorKeepEllipseDefinitionsAndFreshStableIds)
{
    ed::Document doc;
    ed::PrimitiveInput input;
    input.kind = ed::Primitive::Ellipse;
    input.first = { 2, 3, 1 };
    input.second = { 6, 3, 1 };
    input.uniform = true;
    std::string error;
    const ed::Id id = ed::AddPrimitive (doc, input, "circle", error);
    ASSERT_NE (id, ed::kNoId) << error;
    const auto original = *ed::Find (doc, id);
    std::vector<ed::Ref> pasted;
    ASSERT_TRUE (ed::Paste (doc, ed::Copy (doc, { { id, ed::Part::Entity, 0 } }), { 10, -4, 2 }, pasted, error))
        << error;
    ASSERT_EQ (pasted.size (), 1u);
    const auto* copy = ed::Find (doc, pasted.front ().entity);
    ASSERT_NE (copy, nullptr);
    ASSERT_TRUE (copy->ellipse.has_value ());
    EXPECT_EQ (copy->ellipse->mode, ed::EllipseMode::Circle);
    EXPECT_EQ (copy->ellipse->center, (ed::Vec3 { 12, -1, 3 }));
    EXPECT_NE (copy->id, original.id);
    for (size_t i = 0; i < copy->vertices.size (); ++i)
        EXPECT_NE (copy->vertices[i].id, original.vertices[i].id);
    EXPECT_TRUE (ed::Validate (doc, error)) << error;

    const std::vector<ed::Id> ids = { original.vertices[0].id, original.vertices[1].id, original.vertices[2].id,
                                      original.vertices[3].id };
    ASSERT_TRUE (ed::Mirror (doc, { { id, ed::Part::Entity, 0 } }, { 0, 0, 0 }, { 0, 1, 0 }, error)) << error;
    const auto* mirrored = ed::Find (doc, id);
    ASSERT_NE (mirrored, nullptr);
    ASSERT_TRUE (mirrored->ellipse.has_value ());
    EXPECT_EQ (mirrored->ellipse->mode, ed::EllipseMode::Circle);
    EXPECT_EQ (mirrored->ellipse->center, (ed::Vec3 { -2, 3, 1 }));
    for (const ed::Id vertex : ids)
        EXPECT_GE (ed::VertexIndex (*mirrored, vertex), 0);
    EXPECT_TRUE (ed::ValidateEllipse (*mirrored, error)) << error;
    EXPECT_TRUE (ed::Validate (doc, error)) << error;
}

TEST (EditArrange, APastedPathKeepsItsOwnReferencesAndDropsOnesToPathsLeftBehind)
{
    ed::Document doc;
    const ed::Id p = ed::AddPath (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 } }, 1.5);
    const ed::Id q = ed::AddPath (doc, { { 20, 0, 0 }, { 30, 0, 0 } }, 1.5);
    ed::Path& path = *ed::FindPath (doc, p);
    const ed::Id corner = path.points[1].id;
    path.profile.back ().anchor = path.points.back ().id;
    path.profile.back ().binding = "slab:1";
    path.profile.front ().crossing = q;
    path.kerbs.push_back ({ corner, ed::kNoId, ed::KerbSide::Left, ed::KerbSide::Left, 0.4 });
    path.kerbs.push_back ({ ed::kNoId, q, ed::KerbSide::Right, ed::KerbSide::Left, 0.6 });

    std::vector<ed::Ref> pasted;
    std::string error;
    ASSERT_TRUE (ed::Paste (doc, ed::Copy (doc, { { p, ed::Part::Entity, 0 } }), { 0, -20, 5 }, pasted, error))
        << error;
    const ed::Path& copy = doc.paths.back ();
    EXPECT_NE (copy.id, p);
    EXPECT_DOUBLE_EQ (copy.points[1].p.y, -20.0);
    EXPECT_DOUBLE_EQ (copy.points[1].p.z, doc.paths[0].points[1].p.z); // heights stay the profile's
    EXPECT_EQ (copy.profile.back ().anchor, copy.points.back ().id);
    EXPECT_TRUE (copy.profile.back ().binding.empty ());
    EXPECT_EQ (copy.profile.front ().crossing, ed::kNoId);
    ASSERT_EQ (copy.kerbs.size (), 1u); // the junction with q stays behind
    EXPECT_EQ (copy.kerbs[0].point, copy.points[1].id);

    // Pasted together, the two keep their junction with each other.
    ASSERT_TRUE (ed::Paste (doc, ed::Copy (doc, { { p, ed::Part::Entity, 0 }, { q, ed::Part::Entity, 0 } }),
                            { 0, 40, 0 }, pasted, error))
        << error;
    ASSERT_EQ (pasted.size (), 2u);
    const ed::Path& p2 = *ed::FindPath (doc, pasted[0].entity);
    EXPECT_EQ (p2.profile.front ().crossing, pasted[1].entity);
    ASSERT_EQ (p2.kerbs.size (), 2u);
    EXPECT_EQ (p2.kerbs[1].other, pasted[1].entity);
}

TEST (EditArrange, PasteThroughTheSessionIsOneUndoStep)
{
    ed::Document doc;
    const ed::Id a = ed::AddPolyline (doc, ed::Rectangle ({ 0, 0, 0 }, { 2, 2, 0 }), true);
    ed::Session s (doc);
    const auto clip = ed::Copy (s.Doc (), { { a, ed::Part::Entity, 0 } });
    ASSERT_TRUE (s.Apply ([clip] (ed::Document& d, std::string& error) {
        std::vector<ed::Ref> pasted;
        return ed::Paste (d, clip, { 3, 0, 0 }, pasted, error);
    })) << s.Message ();
    EXPECT_EQ (s.Doc ().entities.size (), 2u);
    ASSERT_TRUE (s.Undo ());
    EXPECT_EQ (s.Doc ().entities.size (), 1u);
}

TEST (EditArrange, MirrorKeepsARingsWindingAndALinesDirection)
{
    ed::Document doc;
    const ed::Id ring = ed::AddPolyline (doc, { { 0, 0, 1 }, { 2, 0, 1 }, { 2, 1, 1 } }, true);
    const ed::Id line = ed::AddPolyline (doc, { { 0, 3, 0 }, { 2, 4, 0 } }, false);
    const ed::Id box = ed::AddExtrusion (doc, ed::Rectangle ({ 0, 6, 0 }, { 1, 7, 0 }), 2.0);
    ed::Find (doc, line)->vertices[0].out = { 1, 0, 0 };
    const ed::Id corner = ed::Find (doc, ring)->vertices[1].id;
    std::string error;
    // Across the vertical plane x = 5.
    ASSERT_TRUE (ed::Mirror (doc,
                             { { ring, ed::Part::Entity, 0 },
                               { line, ed::Part::Vertex, ed::Find (doc, line)->vertices[1].id },
                               { box, ed::Part::Entity, 0 } },
                             { 5, 0, 0 }, { 0, 1, 0 }, error))
        << error;
    const ed::Entity& r = *ed::Find (doc, ring);
    EXPECT_GT (ed::SignedAreaXY (r.vertices), 0.0);
    const int i = ed::VertexIndex (r, corner);
    ASSERT_GE (i, 0);
    EXPECT_DOUBLE_EQ (r.vertices[size_t (i)].p.x, 8.0);
    EXPECT_DOUBLE_EQ (r.vertices[size_t (i)].p.z, 1.0);
    const ed::Entity& l = *ed::Find (doc, line);
    EXPECT_DOUBLE_EQ (l.vertices[0].p.x, 10.0); // still its start
    EXPECT_DOUBLE_EQ (l.vertices[0].out.x, -1.0);
    EXPECT_GT (ed::SignedAreaXY (ed::Find (doc, box)->vertices), 0.0);
    Valid (doc);
}

TEST (EditArrange, MirrorRefusesALockedVertexOffTheLine)
{
    ed::Document doc;
    const ed::Id e = ed::AddPolyline (doc, { { 0, 0, 0 }, { 2, 0, 0 } }, false);
    ed::Find (doc, e)->vertices[1].lockXY = true;
    std::string error;
    ed::Document copy = doc;
    EXPECT_FALSE (ed::Mirror (copy, { { e, ed::Part::Entity, 0 } }, { 0, 0, 0 }, { 0, 1, 0 }, error));
    // On the line it does not move, so it may stay locked.
    EXPECT_TRUE (ed::Mirror (doc, { { e, ed::Part::Entity, 0 } }, { 0, 0, 0 }, { 1, 0, 0 }, error)) << error;
    EXPECT_FALSE (ed::Mirror (doc, { { e, ed::Part::Entity, 0 } }, { 0, 0, 0 }, { 0, 0, 1 }, error));
}

TEST (EditArrange, AMirroredPathTradesItsSides)
{
    ed::Document doc;
    const ed::Id p = ed::AddPath (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 } }, 2.0);
    ed::Path& path = *ed::FindPath (doc, p);
    path.sections[0].widthLeft = 2.0;
    path.sections[0].endLeft = 2.0;
    path.sections[0].fall = ed::Fall::Left;
    path.kerbs.push_back ({ path.points[1].id, ed::kNoId, ed::KerbSide::Right, ed::KerbSide::Left, 0.5 });
    path.kerbs.push_back ({ path.points[0].id, ed::kNoId, ed::KerbSide::Start, ed::KerbSide::Left, 0.2 });
    std::string error;
    ASSERT_TRUE (ed::Mirror (doc, { { p, ed::Part::Entity, 0 } }, { 0, 0, 0 }, { 1, 0, 0 }, error)) << error;
    const ed::Path& m = *ed::FindPath (doc, p);
    EXPECT_DOUBLE_EQ (m.points[2].p.y, -10.0);
    EXPECT_DOUBLE_EQ (m.sections[0].widthLeft, 1.0);
    EXPECT_DOUBLE_EQ (m.sections[0].widthRight, 2.0);
    EXPECT_DOUBLE_EQ (m.sections[0].endRight, 2.0);
    EXPECT_EQ (m.sections[0].fall, ed::Fall::Right);
    EXPECT_EQ (m.kerbs[0].side, ed::KerbSide::Left);
    EXPECT_EQ (m.kerbs[1].side, ed::KerbSide::Start);
    Valid (doc);
}
