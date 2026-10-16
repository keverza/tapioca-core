#include "Edit/EditSession.hpp"
#include <gtest/gtest.h>

namespace ed = geomsrv::edit;

namespace {
ed::Document Line (ed::Id& entity)
{
    ed::Document doc;
    entity = ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 } }, false);
    return doc;
}

ed::Operation MoveEnd (ed::Id entity, double dx)
{
    return [=] (ed::Document& d, std::string&) {
        ed::Find (d, entity)->vertices[1].p.x += dx;
        return true;
    };
}

double EndX (const ed::Session& s, ed::Id entity)
{
    return ed::Find (s.Doc (), entity)->vertices[1].p.x;
}
} // namespace

TEST (EditSession, MovesAreEvaluatedFromTheGestureStart)
{
    ed::Id e = 0;
    ed::Session s (Line (e));
    ASSERT_TRUE (s.Begin ());
    ASSERT_TRUE (s.Move (MoveEnd (e, 1.0)));
    ASSERT_TRUE (s.Move (MoveEnd (e, 2.0)));
    EXPECT_DOUBLE_EQ (EndX (s, e), 6.0); // 4 + 2, not 4 + 1 + 2
    ASSERT_TRUE (s.Move (MoveEnd (e, 0.0)));
    EXPECT_DOUBLE_EQ (EndX (s, e), 4.0);
    EXPECT_FALSE (s.End ()); // back at its start: nothing to undo
    EXPECT_FALSE (s.CanUndo ());
}

TEST (EditSession, OneUndoEntryPerGesture)
{
    ed::Id e = 0;
    ed::Session s (Line (e));
    ASSERT_TRUE (s.Begin ());
    for (int i = 1; i <= 5; ++i)
        ASSERT_TRUE (s.Move (MoveEnd (e, i)));
    ASSERT_TRUE (s.End ());
    EXPECT_EQ (s.UndoDepth (), 1u);
    ASSERT_TRUE (s.Undo ());
    EXPECT_DOUBLE_EQ (EndX (s, e), 4.0);
    ASSERT_TRUE (s.Redo ());
    EXPECT_DOUBLE_EQ (EndX (s, e), 9.0);
}

TEST (EditSession, RefusedMoveKeepsTheLastValidState)
{
    ed::Id e = 0;
    ed::Session s (Line (e));
    ASSERT_TRUE (s.Begin ());
    ASSERT_TRUE (s.Move (MoveEnd (e, 1.0)));
    // Collapsing the segment is invalid geometry: refused by validation.
    EXPECT_FALSE (s.Move (MoveEnd (e, -4.0)));
    EXPECT_NE (s.Message ().find ("zero length"), std::string::npos);
    EXPECT_DOUBLE_EQ (EndX (s, e), 5.0);
    // An operation can refuse with its own reason.
    EXPECT_FALSE (s.Move ([] (ed::Document&, std::string& error) {
        error = "locked";
        return false;
    }));
    EXPECT_EQ (s.Message (), "locked");
    EXPECT_DOUBLE_EQ (EndX (s, e), 5.0);
}

TEST (EditSession, CancelRestoresAndRecordsNothing)
{
    ed::Id e = 0;
    ed::Session s (Line (e));
    const uint64_t revision = s.Doc ().revision;
    ASSERT_TRUE (s.Begin ());
    ASSERT_TRUE (s.Move (MoveEnd (e, 3.0)));
    s.Cancel ();
    EXPECT_DOUBLE_EQ (EndX (s, e), 4.0);
    EXPECT_FALSE (s.CanUndo ());
    EXPECT_GT (s.Doc ().revision, revision); // the host must republish the restored state
}

TEST (EditSession, StaleTargetCannotBeginAndGesturesDoNotNest)
{
    ed::Id e = 0;
    ed::Session s (Line (e));
    EXPECT_FALSE (s.Begin ({ e, ed::Part::Vertex, 9999 }));
    EXPECT_FALSE (s.InGesture ());
    ASSERT_TRUE (s.Begin ({ e, ed::Part::Entity, 0 }));
    EXPECT_FALSE (s.Begin ());
    EXPECT_FALSE (s.CanUndo ()); // no undo while a gesture is open
    s.Cancel ();
}

TEST (EditSession, ApplyIsAOneShotGesture)
{
    ed::Id e = 0;
    ed::Session s (Line (e));
    ASSERT_TRUE (s.Apply (MoveEnd (e, 1.0)));
    EXPECT_EQ (s.UndoDepth (), 1u);
    EXPECT_FALSE (s.Apply (MoveEnd (e, -5.0)));
    EXPECT_EQ (s.UndoDepth (), 1u);
    EXPECT_FALSE (s.Message ().empty ());
    EXPECT_FALSE (s.InGesture ());
}

TEST (EditSession, NewEditClearsRedoAndIdsAreNeverReused)
{
    ed::Id e = 0;
    ed::Session s (Line (e));
    ed::Id added = 0;
    ASSERT_TRUE (s.Apply ([&] (ed::Document& d, std::string&) {
        added = ed::AddPolyline (d, { { 0, 1, 0 }, { 1, 1, 0 } }, false);
        return true;
    }));
    ASSERT_TRUE (s.Undo ());
    ASSERT_TRUE (s.Apply (MoveEnd (e, 1.0)));
    EXPECT_FALSE (s.CanRedo ());
    ed::Id again = 0;
    ASSERT_TRUE (s.Apply ([&] (ed::Document& d, std::string&) {
        again = ed::AddPolyline (d, { { 0, 2, 0 }, { 1, 2, 0 } }, false);
        return true;
    }));
    EXPECT_GT (again, added);
}

TEST (EditSession, UndoHistoryIsCapped)
{
    ed::Id e = 0;
    ed::Session s (Line (e), 3);
    for (int i = 0; i < 10; ++i)
        ASSERT_TRUE (s.Apply (MoveEnd (e, 1.0)));
    EXPECT_EQ (s.UndoDepth (), 3u);
}
