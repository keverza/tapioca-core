#include "Edit/EditPolyline.hpp"
#include <gtest/gtest.h>
#include <cmath>

namespace ed = geomsrv::edit;

namespace {
struct Doc {
    ed::Document doc;
    std::string error;
    ed::Entity& E (ed::Id id)
    {
        return *ed::Find (doc, id);
    }
    ed::Id V (ed::Id e, size_t i)
    {
        return E (e).vertices[i].id;
    }
    ed::Id S (ed::Id e, size_t i)
    {
        return E (e).segments[i].id;
    }
    void Valid ()
    {
        std::string why;
        EXPECT_TRUE (ed::Validate (doc, why)) << why;
    }
};

void Near (ed::Vec3 p, double x, double y, double z = 0.0)
{
    EXPECT_NEAR (p.x, x, 1e-9);
    EXPECT_NEAR (p.y, y, 1e-9);
    EXPECT_NEAR (p.z, z, 1e-9);
}
} // namespace

TEST (EditPolyline, InsertSplitsTheSegmentKeepingItsId)
{
    Doc d;
    const ed::Id e = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 4, 0, 0 } }, false);
    d.E (e).segments[0].width = 2.0;
    const ed::Id first = d.S (e, 0);
    ed::Id created = 0;
    ASSERT_TRUE (ed::InsertVertex (d.doc, e, first, { 1, 0, 0 }, d.error, &created)) << d.error;
    ASSERT_EQ (d.E (e).vertices.size (), 3u);
    EXPECT_EQ (d.E (e).vertices[1].id, created);
    EXPECT_EQ (d.S (e, 0), first);
    EXPECT_DOUBLE_EQ (d.E (e).segments[1].width, 2.0);
    d.Valid ();
}

TEST (EditPolyline, InsertOnTheClosingSegment)
{
    Doc d;
    const ed::Id e = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 2, 0, 0 }, { 2, 2, 0 }, { 0, 2, 0 } }, true);
    ASSERT_TRUE (ed::InsertVertex (d.doc, e, d.S (e, 3), { 0, 1, 0 }, d.error));
    Near (d.E (e).vertices.back ().p, 0, 1);
    EXPECT_EQ (d.E (e).segments.size (), 5u);
    d.Valid ();
}

TEST (EditPolyline, DeleteStretchesTheArrivingSegment)
{
    Doc d;
    const ed::Id e = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 1, 1, 0 }, { 2, 0, 0 }, { 3, 0, 0 } }, false);
    const ed::Id arriving = d.S (e, 0);
    ASSERT_TRUE (ed::DeleteVertex (d.doc, e, d.V (e, 1), d.error));
    EXPECT_EQ (d.S (e, 0), arriving);
    Near (d.E (e).vertices[1].p, 2, 0);
    ASSERT_TRUE (ed::DeleteVertex (d.doc, e, d.V (e, 2), d.error)); // the last one
    EXPECT_EQ (d.E (e).vertices.size (), 2u);
    EXPECT_FALSE (ed::DeleteVertex (d.doc, e, d.V (e, 0), d.error));
    d.Valid ();

    const ed::Id ring = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 2, 0, 0 }, { 2, 2, 0 }, { 0, 2, 0 } }, true);
    ASSERT_TRUE (ed::DeleteVertex (d.doc, ring, d.V (ring, 0), d.error));
    EXPECT_EQ (d.E (ring).vertices.size (), 3u);
    EXPECT_FALSE (ed::DeleteVertex (d.doc, ring, d.V (ring, 0), d.error));
    d.Valid ();
}

TEST (EditPolyline, MergeRemovesDuplicatesButKeepsLocks)
{
    Doc d;
    const ed::Id e = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 1, 0, 0 }, { 1.001, 0, 0 }, { 2, 0, 0 } }, false);
    d.E (e).vertices[1].lockZ = true;
    ASSERT_TRUE (ed::MergeVertices (d.doc, e, 0.01, d.error)) << d.error;
    ASSERT_EQ (d.E (e).vertices.size (), 3u);
    EXPECT_TRUE (d.E (e).vertices[1].lockZ); // the locked one stayed
    d.Valid ();
    const ed::Id ring =
        ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 2, 0, 0 }, { 2, 2, 0 }, { 0, 2, 0 }, { 0, 0.001, 0 } }, true);
    ASSERT_TRUE (ed::MergeVertices (d.doc, ring, 0.01, d.error)) << d.error;
    EXPECT_EQ (d.E (ring).vertices.size (), 4u);
    d.Valid ();
}

TEST (EditPolyline, OffsetSegmentKeepsNeighbourDirections)
{
    Doc d;
    // A trapezoid: offsetting the top edge outward slides along the slanted sides.
    const ed::Id e = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 6, 0, 0 }, { 5, 2, 0 }, { 1, 2, 0 } }, true);
    ASSERT_TRUE (ed::OffsetSegment (d.doc, e, d.S (e, 2), -1.0, d.error)) << d.error; // right of (5,2)->(1,2) is +Y
    Near (d.E (e).vertices[2].p, 4.5, 3);
    Near (d.E (e).vertices[3].p, 1.5, 3);
    d.Valid ();
    EXPECT_FALSE (ed::OffsetSegment (d.doc, e, d.S (e, 2), 10.0, d.error)); // would turn inside out
}

TEST (EditPolyline, OffsetEntityCopiesInParallel)
{
    Doc d;
    const ed::Id e = ed::AddPolyline (d.doc, { { 0, 0, 1 }, { 4, 0, 1 }, { 4, 4, 2 } }, false);
    ed::Id copy = 0;
    ASSERT_TRUE (ed::OffsetEntity (d.doc, e, 1.0, d.error, &copy)) << d.error;
    const auto& v = d.E (copy).vertices;
    Near (v[0].p, 0, 1, 1);
    Near (v[1].p, 3, 1, 1); // the inside corner
    Near (v[2].p, 3, 4, 2);
    d.Valid ();
    const ed::Id box = ed::AddExtrusion (d.doc, { { 0, 0, 0 }, { 2, 0, 0 }, { 2, 2, 0 }, { 0, 2, 0 } }, 1.0);
    ed::Id grown = 0;
    ASSERT_TRUE (ed::OffsetEntity (d.doc, box, 0.5, d.error, &grown)) << d.error;
    Near (d.E (grown).vertices[0].p, -0.5, -0.5);
    EXPECT_FALSE (ed::OffsetEntity (d.doc, box, -1.5, d.error)); // shrinks past nothing
}

TEST (EditPolyline, ExtendAndTrimAgainstAnotherEntity)
{
    Doc d;
    const ed::Id wall = ed::AddPolyline (d.doc, { { 10, -5, 0 }, { 10, 5, 0 } }, false);
    const ed::Id path = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 4, 0, 0.2 } }, false);
    ASSERT_TRUE (ed::ExtendToEntity (d.doc, path, false, wall, d.error)) << d.error;
    Near (d.E (path).vertices[1].p, 10, 0, 0.5);                          // the slope carries on
    EXPECT_FALSE (ed::ExtendToEntity (d.doc, path, true, wall, d.error)); // backwards never meets it

    const ed::Id longer = ed::AddPolyline (d.doc, { { 0, 1, 0 }, { 8, 1, 0 }, { 14, 1, 0 } }, false);
    const ed::Id kept = d.S (longer, 1);
    ASSERT_TRUE (ed::TrimAtEntity (d.doc, longer, false, wall, d.error)) << d.error;
    ASSERT_EQ (d.E (longer).vertices.size (), 3u);
    Near (d.E (longer).vertices[2].p, 10, 1);
    EXPECT_EQ (d.S (longer, 1), kept);
    ASSERT_TRUE (
        ed::TrimAtEntity (d.doc, longer, true, ed::AddPolyline (d.doc, { { 3, 0, 0 }, { 3, 2, 0 } }, false), d.error));
    Near (d.E (longer).vertices[0].p, 3, 1);
    d.Valid ();
}

TEST (EditPolyline, SplitOpenAndOpenARing)
{
    Doc d;
    const ed::Id e = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 }, { 3, 0, 0 } }, false);
    ed::Id tail = 0;
    ASSERT_TRUE (ed::SplitAtVertex (d.doc, e, d.V (e, 1), d.error, &tail));
    EXPECT_EQ (d.E (e).vertices.size (), 2u);
    EXPECT_EQ (d.E (tail).vertices.size (), 3u);
    Near (d.E (tail).vertices[0].p, 1, 0);
    d.Valid ();
    EXPECT_FALSE (ed::SplitAtVertex (d.doc, e, d.V (e, 0), d.error));

    const ed::Id ring = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 2, 0, 0 }, { 2, 2, 0 } }, true);
    const ed::Id leaving = d.S (ring, 1);
    ASSERT_TRUE (ed::SplitAtVertex (d.doc, ring, d.V (ring, 1), d.error));
    EXPECT_FALSE (d.E (ring).closed);
    EXPECT_EQ (d.E (ring).vertices.size (), 4u);
    EXPECT_EQ (d.S (ring, 0), leaving);
    Near (d.E (ring).vertices.back ().p, 2, 0);
    d.Valid ();
}

TEST (EditPolyline, JoinInAnyOrientationAndCloseALoop)
{
    Doc d;
    const ed::Id a = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 2, 0, 0 } }, false);
    const ed::Id b = ed::AddPolyline (d.doc, { { 2, 2, 0 }, { 2, 0, 0 } }, false); // reversed
    ASSERT_TRUE (ed::Join (d.doc, a, b, 0.01, d.error)) << d.error;
    EXPECT_EQ (ed::Find (d.doc, b), nullptr);
    ASSERT_EQ (d.E (a).vertices.size (), 3u);
    Near (d.E (a).vertices[2].p, 2, 2);
    d.Valid ();
    const ed::Id c = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 0, 2, 0 }, { 2, 2, 0 } }, false);
    ASSERT_TRUE (ed::Join (d.doc, a, c, 0.01, d.error)) << d.error;
    EXPECT_TRUE (d.E (a).closed);
    EXPECT_EQ (d.E (a).vertices.size (), 4u);
    d.Valid ();
    const ed::Id far = ed::AddPolyline (d.doc, { { 9, 9, 0 }, { 10, 9, 0 } }, false);
    const ed::Id other = ed::AddPolyline (d.doc, { { 0, 9, 0 }, { 1, 9, 0 } }, false);
    EXPECT_FALSE (ed::Join (d.doc, far, other, 0.01, d.error));
}

TEST (EditPolyline, FilletReplacesTheCornerWithATangentArc)
{
    Doc d;
    const ed::Id e = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 4, 2 } }, false);
    const ed::Id corner = d.V (e, 1), leaving = d.S (e, 1);
    ASSERT_TRUE (ed::Fillet (d.doc, e, corner, 1.0, 4, d.error)) << d.error;
    const auto& v = d.E (e).vertices;
    ASSERT_EQ (v.size (), 7u);
    EXPECT_EQ (v[1].id, corner);
    Near (v[1].p, 3, 0, 0);
    EXPECT_NEAR (v[5].p.x, 4.0, 1e-9);
    EXPECT_NEAR (v[5].p.y, 1.0, 1e-9);
    EXPECT_NEAR (v[5].p.z, 0.5, 1e-9); // heights follow their segments
    for (size_t i = 1; i <= 5; ++i)    // every arc point sits on the circle about (3,1)
        EXPECT_NEAR (std::hypot (v[i].p.x - 3.0, v[i].p.y - 1.0), 1.0, 1e-9);
    EXPECT_EQ (d.E (e).segments.back ().id, leaving);
    d.Valid ();
    EXPECT_FALSE (ed::Fillet (d.doc, e, d.V (e, 3), 50.0, 4, d.error)); // does not fit
    EXPECT_FALSE (ed::Fillet (d.doc, e, d.V (e, 0), 0.5, 4, d.error));  // an end
}

TEST (EditPolyline, ChamferCutsTheCorner)
{
    Doc d;
    const ed::Id e = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 3, 0, 0 }, { 3, 3, 0 }, { 0, 3, 0 } }, true);
    ASSERT_TRUE (ed::Chamfer (d.doc, e, d.V (e, 0), 1.0, d.error)) << d.error;
    EXPECT_EQ (d.E (e).vertices.size (), 5u);
    Near (d.E (e).vertices[0].p, 0, 1);
    Near (d.E (e).vertices[1].p, 1, 0);
    d.Valid ();
}

TEST (EditPolyline, SetCoordinateAlignsSelectedPoints)
{
    Doc d;
    const ed::Id e = ed::AddPolyline (d.doc, { { 0, 0, 1 }, { 4, 0, 2 }, { 8, 0, 3 } }, false);
    ASSERT_TRUE (ed::SetCoordinate (d.doc, { { e, ed::Part::Vertex, d.V (e, 0) }, { e, ed::Part::Vertex, d.V (e, 2) } },
                                    2, 5.0, d.error));
    EXPECT_DOUBLE_EQ (d.E (e).vertices[0].p.z, 5.0);
    EXPECT_DOUBLE_EQ (d.E (e).vertices[1].p.z, 2.0);
    EXPECT_DOUBLE_EQ (d.E (e).vertices[2].p.z, 5.0);
    const ed::Id box = ed::AddExtrusion (d.doc, { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 } }, 1.0);
    EXPECT_FALSE (ed::SetCoordinate (d.doc, { { box, ed::Part::Vertex, d.V (box, 0) } }, 2, 3.0, d.error));
    EXPECT_TRUE (ed::SetCoordinate (d.doc, { { box, ed::Part::Entity, 0 } }, 2, 3.0, d.error));
}

TEST (EditPolyline, ProjectOntoASurfaceKeepsFixedElevations)
{
    Doc d;
    const ed::Id e = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 5, 0, 0 }, { 10, 0, 0 } }, false);
    d.E (e).vertices[2].lockZ = true;
    const ed::HeightField slope = [] (double x, double, double& z) {
        z = 100.0 + 0.05 * x;
        return x >= 0.0 && x <= 20.0;
    };
    ASSERT_TRUE (ed::ProjectToSurface (d.doc, e, slope, d.error)) << d.error;
    EXPECT_DOUBLE_EQ (d.E (e).vertices[1].p.z, 100.25);
    EXPECT_DOUBLE_EQ (d.E (e).vertices[2].p.z, 0.0);
    d.E (e).vertices[0].p.x = -1.0;
    EXPECT_FALSE (ed::ProjectToSurface (d.doc, e, slope, d.error));
}

TEST (EditPolyline, ActionTableRunsThroughTheSession)
{
    Doc d;
    const ed::Id e = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 4, 0 } }, false);
    const auto table = ed::PolylineActions ();
    ed::Selection sel;
    sel.Set ({ { e, ed::Part::Segment, d.S (e, 0) } });
    const auto actions = ed::Applicable (table, sel, ed::ViewKind::Plan);
    ASSERT_FALSE (actions.empty ());
    EXPECT_EQ (actions[0]->id, "insert-vertex");
    ed::Session s (d.doc);
    ASSERT_TRUE (s.Apply (actions[0]->make (s.Doc (), sel.Items ())));
    EXPECT_EQ (ed::Find (s.Doc (), e)->vertices.size (), 4u);
    sel.Set ({ { e, ed::Part::Vertex, ed::Find (s.Doc (), e)->vertices[2].id } });
    std::vector<std::string> ids;
    for (const auto* a : ed::Applicable (table, sel, ed::ViewKind::Plan))
        ids.push_back (a->id);
    EXPECT_EQ (ids, (std::vector<std::string> { "delete-vertex", "fillet", "sharpen", "smooth", "corner", "chamfer",
                                                "split" }));
    ASSERT_TRUE (s.Undo ());
    EXPECT_EQ (ed::Find (s.Doc (), e)->vertices.size (), 3u);
}

TEST (EditPolyline, ReverseKeepsTheShapeAndEveryIdOnItsItem)
{
    Doc d;
    const ed::Id e = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 4, 0 } }, false);
    d.E (e).vertices[0].out = { 1, 1, 0 };
    d.E (e).segments[0].width = 2.0;
    const ed::Id first = d.V (e, 0), firstSegment = d.S (e, 0);
    ASSERT_TRUE (ed::ReverseDirection (d.doc, e, d.error)) << d.error;
    EXPECT_EQ (d.V (e, 2), first);
    Near (d.E (e).vertices[2].in, 1, 1);  // the handle still shapes the same segment
    EXPECT_EQ (d.S (e, 1), firstSegment); // which now runs from vertex 1 to vertex 2
    EXPECT_DOUBLE_EQ (d.E (e).segments[1].width, 2.0);
    d.Valid ();
}

TEST (EditPolyline, ReverseOnARingKeepsEachSegmentBetweenItsVertices)
{
    Doc d;
    const ed::Id e = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 2, 0, 0 }, { 2, 2, 0 }, { 0, 2, 0 } }, true);
    std::vector<std::pair<ed::Vec3, ed::Vec3>> before; // each segment's ends, by its id
    const auto ends = [&] (ed::Id segment) {
        const ed::Entity& x = d.E (e);
        size_t from = 0, to = 0;
        ed::SegmentEnds (x, size_t (ed::SegmentIndex (x, segment)), from, to);
        return std::pair { x.vertices[from].p, x.vertices[to].p };
    };
    std::vector<ed::Id> ids;
    for (size_t i = 0; i < 4; ++i)
        ids.push_back (d.S (e, i)), before.push_back (ends (d.S (e, i)));
    ASSERT_TRUE (ed::ReverseDirection (d.doc, e, d.error)) << d.error;
    EXPECT_LT (ed::SignedAreaXY (d.E (e).vertices), 0.0);
    for (size_t i = 0; i < 4; ++i) {
        const auto [a, b] = ends (ids[i]);
        Near (a, before[i].second.x, before[i].second.y);
        Near (b, before[i].first.x, before[i].first.y);
    }
    d.Valid ();
}

TEST (EditPolyline, CloseAddsTheClosingSegmentOrMergesAnEndOnTheStart)
{
    Doc d;
    const ed::Id e = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 2, 0, 0 }, { 2, 2, 0 } }, false);
    ASSERT_TRUE (ed::Close (d.doc, e, d.error)) << d.error;
    EXPECT_TRUE (d.E (e).closed);
    EXPECT_EQ (d.E (e).segments.size (), 3u);
    d.Valid ();
    EXPECT_FALSE (ed::Close (d.doc, e, d.error));

    const ed::Id back = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 2, 0, 0 }, { 2, 2, 0 }, { 0, 0, 0 } }, false);
    const ed::Id last = d.S (back, 2);
    ASSERT_TRUE (ed::Close (d.doc, back, d.error)) << d.error;
    EXPECT_EQ (d.E (back).vertices.size (), 3u);
    EXPECT_EQ (d.S (back, 2), last);
    d.Valid ();

    const ed::Id line = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 2, 0, 0 } }, false);
    EXPECT_FALSE (ed::Close (d.doc, line, d.error));
}

TEST (EditPolyline, OpenRemovesTheClosingSegment)
{
    Doc d;
    const ed::Id e = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 2, 0, 0 }, { 2, 2, 0 }, { 0, 2, 0 } }, true);
    const ed::Id kept = d.S (e, 2);
    ASSERT_TRUE (ed::Open (d.doc, e, d.error)) << d.error;
    EXPECT_FALSE (d.E (e).closed);
    EXPECT_EQ (d.E (e).segments.size (), 3u);
    EXPECT_EQ (d.S (e, 2), kept);
    d.Valid ();
    EXPECT_FALSE (ed::Open (d.doc, e, d.error));
}

TEST (EditPolyline, CloseOpenAndReverseAreActionsOnWholePolylines)
{
    Doc d;
    const ed::Id open = ed::AddPolyline (d.doc, { { 0, 0, 0 }, { 2, 0, 0 }, { 2, 2, 0 } }, false);
    const ed::Id box = ed::AddExtrusion (d.doc, ed::Rectangle ({ 5, 0, 0 }, { 6, 1, 0 }), 1.0);
    const auto table = ed::PolylineActions ();
    const auto make = [&] (const std::string& id, std::vector<ed::Ref> refs) {
        for (const auto& a : table)
            if (a.id == id)
                return a.make (d.doc, refs);
        return ed::Operation {};
    };
    const ed::Ref o { open, ed::Part::Entity, 0 }, b { box, ed::Part::Entity, 0 };
    EXPECT_FALSE (make ("open", { o }));
    EXPECT_FALSE (make ("reverse", { b })); // an extrusion's profile keeps its winding
    ed::Session s (d.doc);
    ASSERT_TRUE (s.Apply (make ("close", { o, b })));
    EXPECT_TRUE (ed::Find (s.Doc (), open)->closed);
    d.doc = s.Doc ();
    EXPECT_FALSE (make ("close", { o }));
    ASSERT_TRUE (s.Apply (make ("open", { o })));
    EXPECT_FALSE (ed::Find (s.Doc (), open)->closed);
}
