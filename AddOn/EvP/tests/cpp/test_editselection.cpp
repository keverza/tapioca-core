#include "Edit/EditSelection.hpp"
#include "Edit/EditPathEdit.hpp"
#include <gtest/gtest.h>

namespace ed = geomsrv::edit;

namespace {
// Plan view: 100 pixels per metre, looking down.
ed::PickView Plan ()
{
    ed::PickView v;
    v.toScreen = [] (ed::Vec3 p, double& x, double& y) {
        x = p.x * 100.0;
        y = p.y * 100.0;
        return true;
    };
    return v;
}

// The same view at another scale: only the projection moved, nothing of the document.
ed::PickView Plan (double scale)
{
    ed::PickView v;
    v.toScreen = [scale] (ed::Vec3 p, double& x, double& y) {
        x = p.x * scale;
        y = p.y * scale;
        return true;
    };
    return v;
}

bool SameHits (const std::vector<ed::PickHit>& a, const std::vector<ed::PickHit>& b)
{
    if (a.size () != b.size ())
        return false;
    for (size_t i = 0; i < a.size (); ++i)
        if (!(a[i].ref == b[i].ref) || a[i].distance != b[i].distance)
            return false;
    return true;
}

struct Scene {
    ed::Document doc;
    ed::Id line = 0, square = 0, box = 0;
    Scene ()
    {
        line = ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 } }, false);
        square = ed::AddPolyline (doc, { { 10, 0, 0 }, { 12, 0, 0 }, { 12, 2, 0 }, { 10, 2, 0 } }, true);
        box = ed::AddExtrusion (doc, { { 20, 0, 0 }, { 23, 0, 0 }, { 23, 3, 0 }, { 20, 3, 0 } }, 2.0);
    }
};
} // namespace

TEST (EditSelection, VertexBeatsSegmentBeatsEntity)
{
    Scene s;
    const auto hits = ed::Pick (s.doc, Plan (), 401, 2, ed::kPickAll, 8);
    ASSERT_GE (hits.size (), 3u);
    EXPECT_EQ (hits[0].ref.part, ed::Part::Vertex);
    EXPECT_EQ (hits[1].ref.part, ed::Part::Segment);
    EXPECT_EQ (hits.back ().ref.part, ed::Part::Entity);
    // Filters narrow the candidates.
    const auto segs = ed::Pick (s.doc, Plan (), 401, 2, ed::kPickSegment, 8);
    ASSERT_EQ (segs.size (), 1u);
    EXPECT_EQ (segs[0].ref.part, ed::Part::Segment);
}

TEST (EditSelection, ClickInsideAClosedOutlineHitsTheEntity)
{
    Scene s;
    const auto hits = ed::Pick (s.doc, Plan (), 1100, 100, ed::kPickAll, 8);
    ASSERT_EQ (hits.size (), 1u);
    EXPECT_EQ (hits[0].ref, (ed::Ref { s.square, ed::Part::Entity, 0 }));
    EXPECT_TRUE (ed::Pick (s.doc, Plan (), 200, 100, ed::kPickAll, 8).empty ()); // open line has no inside
}

TEST (EditSelection, OnlyFacesTurnedToTheViewArePicked)
{
    Scene s;
    const auto hits = ed::Pick (s.doc, Plan (), 2150, 150, ed::kPickFace, 8);
    ASSERT_EQ (hits.size (), 1u);
    EXPECT_EQ (hits[0].ref.item, 1u); // the top, not the bottom
    ed::PickView below = Plan ();
    below.viewDirection = { 0, 0, 1 };
    const auto under = ed::Pick (s.doc, below, 2150, 150, ed::kPickFace, 8);
    ASSERT_EQ (under.size (), 1u);
    EXPECT_EQ (under[0].ref.item, 0u);
}

TEST (EditSelection, RepeatedClickCyclesStackedCandidates)
{
    Scene s;
    ed::Selection sel;
    const auto hits = ed::Pick (s.doc, Plan (), 401, 2, ed::kPickAll, 8);
    sel.Click (hits, 401, 2, false);
    EXPECT_EQ (sel.Kind (), ed::SelectionKind::Vertex);
    sel.Click (hits, 402, 2, false);
    EXPECT_EQ (sel.Kind (), ed::SelectionKind::Segment);
    sel.Click (hits, 401, 3, false);
    EXPECT_EQ (sel.Kind (), ed::SelectionKind::Entity);
    sel.Click (hits, 401, 2, false);
    EXPECT_EQ (sel.Kind (), ed::SelectionKind::Vertex);
    // A click elsewhere starts at the best hit again; empty space clears.
    sel.Click (ed::Pick (s.doc, Plan (), 1100, 100, ed::kPickAll, 8), 1100, 100, false);
    EXPECT_EQ (sel.Kind (), ed::SelectionKind::Entity);
    sel.Click ({}, 5000, 5000, false);
    EXPECT_EQ (sel.Kind (), ed::SelectionKind::None);
}

TEST (EditSelection, AdditiveClickTogglesAndMixesKinds)
{
    Scene s;
    ed::Selection sel;
    sel.Click (ed::Pick (s.doc, Plan (), 1, 1, ed::kPickAll, 8), 1, 1, false);
    sel.Click (ed::Pick (s.doc, Plan (), 1100, 100, ed::kPickAll, 8), 1100, 100, true);
    EXPECT_EQ (sel.Items ().size (), 2u);
    EXPECT_EQ (sel.Kind (), ed::SelectionKind::Mixed);
    sel.Click (ed::Pick (s.doc, Plan (), 1100, 100, ed::kPickAll, 8), 1100, 100, true);
    EXPECT_EQ (sel.Items ().size (), 1u);
}

TEST (EditSelection, MarqueeContainedVersusCrossing)
{
    Scene s;
    ed::Selection sel;
    // Left to right around the square only.
    sel.Marquee (s.doc, Plan (), 950, -50, 1250, 250, ed::kPickEntity, false);
    ASSERT_EQ (sel.Items ().size (), 1u);
    EXPECT_EQ (sel.Items ()[0].entity, s.square);
    // Left to right over half the line: nothing fully inside.
    sel.Marquee (s.doc, Plan (), 300, -50, 600, 50, ed::kPickEntity, false);
    EXPECT_TRUE (sel.Items ().empty ());
    // Right to left: touching is enough, even with no vertex inside.
    sel.Marquee (s.doc, Plan (), 300, -50, 100, 50, ed::kPickEntity, false);
    ASSERT_EQ (sel.Items ().size (), 1u);
    EXPECT_EQ (sel.Items ()[0].entity, s.line);
    sel.Marquee (s.doc, Plan (), 950, -50, 1250, 250, ed::kPickVertex, false);
    EXPECT_EQ (sel.Items ().size (), 4u);
}

TEST (EditSelection, PruneDropsWhatAnEditRemoved)
{
    Scene s;
    ed::Selection sel;
    sel.Set ({ { s.line, ed::Part::Entity, 0 }, { s.square, ed::Part::Entity, 0 } });
    sel.hover = { s.square, ed::Part::Entity, 0 };
    s.doc.entities.erase (s.doc.entities.begin () + 1);
    sel.Prune (s.doc);
    ASSERT_EQ (sel.Items ().size (), 1u);
    EXPECT_EQ (sel.hover.entity, ed::kNoId);
}

TEST (EditSelection, ActionsComeFromTheTable)
{
    Scene s;
    std::vector<ed::Action> table;
    table.push_back ({ "insert", "Insert vertex", ed::Bit (ed::SelectionKind::Segment), 0xF, 1, 1, {} });
    table.push_back ({ "join", "Join", ed::Bit (ed::SelectionKind::Entity), 0xF, 2, 0, {} });
    table.push_back (
        { "slope", "Slope", ed::Bit (ed::SelectionKind::Segment), ed::Bit (ed::ViewKind::Profile), 1, 0, {} });
    ed::Selection sel;
    sel.Set ({ { s.line, ed::Part::Segment, ed::Find (s.doc, s.line)->segments[0].id } });
    auto plan = ed::Applicable (table, sel, ed::ViewKind::Plan);
    ASSERT_EQ (plan.size (), 1u);
    EXPECT_EQ (plan[0]->id, "insert");
    EXPECT_EQ (ed::Applicable (table, sel, ed::ViewKind::Profile).size (), 2u);
    sel.Set ({ { s.line, ed::Part::Entity, 0 } });
    EXPECT_TRUE (ed::Applicable (table, sel, ed::ViewKind::Plan).empty ()); // join needs two
    sel.Toggle ({ s.square, ed::Part::Entity, 0 });
    EXPECT_EQ (ed::Applicable (table, sel, ed::ViewKind::Plan).size (), 1u);
}

TEST (EditSelection, ACachedPickReusesPathsOutlineAcrossHoverAndNavigation)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 } }, false);
    ed::AddPath (doc, { { 10, 0, 0 }, { 20, 0, 0 } }, 2.0, "kerb");
    ed::PickGeometryCache cache;
    const auto first = ed::Pick (doc, Plan (), 1500, 0, ed::kPickAll, 8, &cache);
    EXPECT_FALSE (first.empty ());
    EXPECT_EQ (cache.builds, doc.paths.size ()); // one outline per path, deterministically
    EXPECT_EQ (cache.entries.size (), doc.paths.size ());
    const uint64_t builds = cache.builds;
    // The same hover again: the outline is reused and the hits are identical.
    const auto again = ed::Pick (doc, Plan (), 1500, 0, ed::kPickAll, 8, &cache);
    EXPECT_EQ (cache.builds, builds);
    EXPECT_TRUE (SameHits (first, again));
    // Navigation: a different projector changes the projection, never the tessellation.
    const auto moved = ed::Pick (doc, Plan (200.0), 3000, 0, ed::kPickAll, 16, &cache);
    EXPECT_EQ (cache.builds, builds);
    // Cached and uncached agree at the new projector, hit for hit.
    const auto plain = ed::Pick (doc, Plan (200.0), 3000, 0, ed::kPickAll, 16);
    EXPECT_TRUE (SameHits (moved, plain));
}

TEST (EditSelection, ACachedPickRebuildsOnlyWhenTheDocumentMoves)
{
    ed::Document doc;
    const ed::Id path = ed::AddPath (doc, { { 0, 0, 0 }, { 10, 0, 0 } }, 2.0, "kerb");
    const ed::Id tip = doc.paths.front ().points[1].id;
    ed::PickGeometryCache cache;
    ed::Pick (doc, Plan (), 500, 0, ed::kPickAll, 8, &cache);
    const uint64_t builds = cache.builds;
    ed::Pick (doc, Plan (), 500, 0, ed::kPickAll, 8, &cache);
    EXPECT_EQ (cache.builds, builds);
    // A no-op Move leaves the revision alone (EditSession): nothing is rebuilt.
    ed::Session session (doc);
    ASSERT_TRUE (session.Begin ());
    ASSERT_TRUE (session.Move ([] (ed::Document&, std::string&) { return true; }));
    session.End ();
    ed::Pick (session.Doc (), Plan (), 500, 0, ed::kPickAll, 8, &cache);
    EXPECT_EQ (cache.builds, builds);
    // A real edit moves the revision: the next pick rebuilds.
    ASSERT_TRUE (session.Apply ([path, tip] (ed::Document& d, std::string& error) {
        return ed::MovePathPoint (d, path, tip, { 12, 0, 0 }, error);
    }));
    ed::Pick (session.Doc (), Plan (), 500, 0, ed::kPickAll, 8, &cache);
    EXPECT_GT (cache.builds, builds);
}

TEST (EditSelection, ACachedPickIsNotStaleForAReplacedDocument)
{
    ed::Document doc;
    ed::AddPath (doc, { { 0, 0, 0 }, { 10, 0, 0 } }, 2.0, "kerb");
    ed::PickGeometryCache cache;
    EXPECT_FALSE (ed::Pick (doc, Plan (), 500, 0, ed::kPickAll, 8, &cache).empty ());
    const uint64_t builds = cache.builds;
    // A different document that keeps the revision and the path's ids, moved elsewhere.
    ed::Document other = doc;
    other.paths.front ().points[0].p = { 100, 0, 0 };
    other.paths.front ().points[1].p = { 110, 0, 0 };
    ASSERT_EQ (other.revision, doc.revision);
    ASSERT_EQ (other.paths.front ().id, doc.paths.front ().id);
    // The stale outline must not answer for the moved path...
    EXPECT_TRUE (ed::Pick (other, Plan (), 500, 0, ed::kPickAll, 8, &cache).empty ());
    // ... and the new shape is found where it now is.
    EXPECT_FALSE (ed::Pick (other, Plan (), 10500, 0, ed::kPickAll, 8, &cache).empty ());
    EXPECT_GT (cache.builds, builds); // the value changed: one rebuild, no stale hit
}

TEST (EditSelection, ACachedPickDropsAPathTheDocumentNoLongerHas)
{
    ed::Document doc;
    ed::AddPath (doc, { { 0, 0, 0 }, { 10, 0, 0 } }, 2.0, "kerb");
    ed::AddPath (doc, { { 0, 20, 0 }, { 10, 20, 0 } }, 2.0, "kerb");
    ed::PickGeometryCache cache;
    ed::Pick (doc, Plan (), 500, 0, ed::kPickAll, 8, &cache);
    EXPECT_EQ (cache.entries.size (), 2u);
    const uint64_t builds = cache.builds;
    doc.paths.pop_back (); // a path leaves without the revision moving
    ed::Pick (doc, Plan (), 500, 0, ed::kPickAll, 8, &cache);
    EXPECT_EQ (cache.entries.size (), 1u); // pruned to what the document still has
    EXPECT_EQ (cache.builds, builds);      // the survivor's outline was reused
}

TEST (EditSelection, ACachedPickRebuildsAfterUndo)
{
    ed::Document doc;
    const ed::Id path = ed::AddPath (doc, { { 0, 0, 0 }, { 10, 0, 0 } }, 2.0, "kerb");
    const ed::Id tip = doc.paths.front ().points[1].id;
    ed::Session session (doc);
    ed::PickGeometryCache cache;
    ed::Pick (session.Doc (), Plan (), 500, 0, ed::kPickAll, 8, &cache);
    const uint64_t builds = cache.builds;
    ASSERT_TRUE (session.Apply ([path, tip] (ed::Document& d, std::string& error) {
        return ed::MovePathPoint (d, path, tip, { 12, 0, 0 }, error);
    }));
    ed::Pick (session.Doc (), Plan (), 500, 0, ed::kPickAll, 8, &cache);
    const uint64_t afterEdit = cache.builds;
    EXPECT_GT (afterEdit, builds);
    ASSERT_TRUE (session.Undo ()); // the restore moves the revision as well
    ed::Pick (session.Doc (), Plan (), 500, 0, ed::kPickAll, 8, &cache);
    EXPECT_GT (cache.builds, afterEdit);
}

// The user, 2026-10-24: lines are hard to select where regions overlap, circles especially.
// The pointer inside one closed outline used to hit it at distance 0, beating the outline whose
// edge was right under the pointer.
TEST (EditSelection, AnEdgeUnderThePointerBeatsTheRegionItIsInside)
{
    ed::Document doc;
    const auto a = ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 4, 0 }, { 0, 4, 0 } }, true);
    const auto b = ed::AddPolyline (doc, { { 2, 0, 0 }, { 6, 0, 0 }, { 6, 4, 0 }, { 2, 4, 0 } }, true);
    // 3 px right of B's left edge, deep inside A.
    const auto hits = ed::Pick (doc, Plan (), 203, 150, ed::kPickEntity, 8);
    ASSERT_EQ (hits.size (), 2u);
    EXPECT_EQ (hits[0].ref.entity, b);
    EXPECT_FALSE (hits[0].inside);
    EXPECT_EQ (hits[1].ref.entity, a);
    EXPECT_TRUE (hits[1].inside);
    EXPECT_GT (hits[1].distance, 8.0);
}

TEST (EditSelection, OfNestedRegionsTheInnermostIsPickedInside)
{
    ed::Document doc;
    const auto outer = ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } }, true);
    const auto inner = ed::AddPolyline (doc, { { 4, 4, 0 }, { 6, 4, 0 }, { 6, 6, 0 }, { 4, 6, 0 } }, true);
    for (const auto order : { 0, 1 }) {
        (void) order;
        const auto hits = ed::Pick (doc, Plan (), 500, 500, ed::kPickAll, 8);
        ASSERT_EQ (hits.size (), 2u);
        EXPECT_EQ (hits[0].ref.entity, inner);
        EXPECT_EQ (hits[1].ref.entity, outer);
    }
}

TEST (EditSelection, AMarqueeWithEveryFilterSelectsWholeObjects)
{
    Scene s;
    ed::Selection sel;
    sel.Marquee (s.doc, Plan (), -50, -50, 1250, 250, ed::kPickAll, false);
    ASSERT_EQ (sel.Items ().size (), 2u);
    EXPECT_EQ (sel.Kind (), ed::SelectionKind::Entity);
    EXPECT_TRUE (sel.Contains ({ s.line, ed::Part::Entity, 0 }));
    EXPECT_TRUE (sel.Contains ({ s.square, ed::Part::Entity, 0 }));
}

TEST (EditSelection, ALassoSelectsWhatLiesInsideItsOutline)
{
    Scene s;
    ed::Selection sel;
    // A triangle around the square (x 1000..1200, y 0..200), clear of the line and the box.
    sel.Lasso (s.doc, Plan (), { { 900, -100 }, { 1500, -100 }, { 1000, 600 } }, ed::kPickAll, false);
    ASSERT_EQ (sel.Items ().size (), 1u);
    EXPECT_EQ (sel.Items ()[0], (ed::Ref { s.square, ed::Part::Entity, 0 }));
    // A notch that cuts a corner of the square off selects nothing, not part of it.
    sel.Lasso (s.doc, Plan (), { { 900, -100 }, { 1150, -100 }, { 1000, 600 } }, ed::kPickAll, false);
    EXPECT_TRUE (sel.Items ().empty ());
    // Parts only when the filter excludes whole objects.
    sel.Lasso (s.doc, Plan (), { { 900, -100 }, { 1150, -100 }, { 1000, 600 } }, ed::kPickVertex, false);
    EXPECT_EQ (sel.Items ().size (), 2u);
    sel.Lasso (s.doc, Plan (), { { 900, -100 }, { 1500, -100 } }, ed::kPickAll, false);
    EXPECT_TRUE (sel.Items ().empty ());
}
