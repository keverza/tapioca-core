#include "Edit/EditSelection.hpp"
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
