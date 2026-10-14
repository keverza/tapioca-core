#include "Edit/EditTracker.hpp"
#include <gtest/gtest.h>

namespace ed = geomsrv::edit;
using K = ed::TrackerKey::Kind;

namespace {
ed::TrackerKey Ch (char c)
{
    return { K::Char, c };
}

void Type (ed::Tracker& t, const std::string& text)
{
    for (char c : text)
        t.Key (Ch (c));
}

std::vector<ed::TrackerItem> LengthAngle (double length = 3.0, double angle = 0.0)
{
    return { { ed::Quantity::Length, length }, { ed::Quantity::Angle, angle } };
}

struct Line {
    ed::Document doc;
    ed::Id e = 0;
    ed::Ref seg;
    explicit Line (ed::Vec3 a = { 0, 0, 10 }, ed::Vec3 b = { 3, 4, 10 })
    {
        e = ed::AddPolyline (doc, { a, b }, false);
        seg = { e, ed::Part::Segment, ed::Find (doc, e)->segments[0].id };
    }
    ed::Vec3 P (size_t i) const
    {
        return ed::Find (doc, e)->vertices[i].p;
    }
};
} // namespace

TEST (EditTracker, ParsesPlainNumbersOnly)
{
    double v = 0;
    EXPECT_TRUE (ed::ParseNumber ("12.5", v));
    EXPECT_DOUBLE_EQ (v, 12.5);
    EXPECT_TRUE (ed::ParseNumber (" -0,75 ", v));
    EXPECT_DOUBLE_EQ (v, -0.75);
    EXPECT_TRUE (ed::ParseNumber ("+3", v));
    for (const char* bad : { "", "-", ".", "1.2.3", "5m", "1:20", "2+3", "1e5" })
        EXPECT_FALSE (ed::ParseNumber (bad, v)) << bad;
}

TEST (EditTracker, TypingStartsOnTheDefaultFieldAndEnterCommits)
{
    ed::Tracker t;
    t.Show (LengthAngle ());
    EXPECT_EQ (t.State (), ed::TrackerState::Showing);
    Type (t, "4.2");
    EXPECT_EQ (t.State (), ed::TrackerState::Editing);
    EXPECT_EQ (t.Text (), "4.2");
    const auto ev = t.Key ({ K::Enter });
    EXPECT_EQ (ev.outcome, ed::Tracker::Outcome::Commit);
    EXPECT_EQ (ev.item, 0u);
    EXPECT_DOUBLE_EQ (ev.value, 4.2);
    EXPECT_EQ (t.State (), ed::TrackerState::Showing);
}

TEST (EditTracker, TabLocksAndMovesOnSoTheDragKeepsTheValue)
{
    ed::Tracker t;
    t.Show (LengthAngle ());
    Type (t, "5");
    const auto ev = t.Key ({ K::Tab });
    EXPECT_EQ (ev.outcome, ed::Tracker::Outcome::Locked);
    EXPECT_EQ (t.Focus (), 1u);
    ASSERT_TRUE (t.Locked (ed::Quantity::Length).has_value ());
    EXPECT_DOUBLE_EQ (*t.Locked (ed::Quantity::Length), 5.0);
    // A pointer move refreshes live values without losing focus or locks.
    t.Show (LengthAngle (7.0, 30.0));
    EXPECT_EQ (t.Focus (), 1u);
    EXPECT_DOUBLE_EQ (t.Items ()[1].value, 30.0);
    EXPECT_TRUE (t.Locked (ed::Quantity::Length).has_value ());
    // A different set of fields starts over.
    t.Show ({ { ed::Quantity::Elevation, 1.0 } });
    EXPECT_FALSE (t.Locked (ed::Quantity::Length).has_value ());
}

TEST (EditTracker, EscapeLeavesTheFieldThenTheGesture)
{
    ed::Tracker t;
    t.Show (LengthAngle ());
    Type (t, "9");
    EXPECT_EQ (t.Key ({ K::Escape }).outcome, ed::Tracker::Outcome::None);
    EXPECT_EQ (t.State (), ed::TrackerState::Showing);
    EXPECT_EQ (t.Key ({ K::Escape }).outcome, ed::Tracker::Outcome::CancelGesture);
}

TEST (EditTracker, InvalidTextStaysInTheField)
{
    ed::Tracker t;
    t.Show (LengthAngle ());
    Type (t, "-x");
    EXPECT_EQ (t.Key ({ K::Enter }).outcome, ed::Tracker::Outcome::Invalid);
    EXPECT_EQ (t.State (), ed::TrackerState::Editing);
    t.Key ({ K::Backspace });
    t.Key ({ K::Backspace });
    EXPECT_EQ (t.State (), ed::TrackerState::Showing);
}

TEST (EditTracker, ReadOnlyFieldsAreSkipped)
{
    ed::Tracker t;
    t.Show ({ { ed::Quantity::Length, 1.0, false }, { ed::Quantity::Slope, 2.0 } });
    EXPECT_EQ (t.Focus (), 1u);
    t.Key ({ K::Tab });
    EXPECT_EQ (t.Focus (), 1u);
}

TEST (EditDimension, SegmentDimensionsReadPlanLengthSlopeAndAngle)
{
    Line l ({ 0, 0, 10 }, { 3, 4, 10.25 });
    double v = 0;
    ASSERT_TRUE (ed::ReadDimension (l.doc, { l.seg, ed::Quantity::Length }, v));
    EXPECT_DOUBLE_EQ (v, 5.0);
    ASSERT_TRUE (ed::ReadDimension (l.doc, { l.seg, ed::Quantity::Slope }, v));
    EXPECT_DOUBLE_EQ (v, 5.0);
    ASSERT_TRUE (ed::ReadDimension (l.doc, { l.seg, ed::Quantity::Angle }, v));
    EXPECT_NEAR (v, 53.130102354, 1e-6);
    const auto dims = ed::DimensionsFor (l.doc, l.seg);
    ASSERT_EQ (dims.size (), 3u); // no width until the segment has one
    EXPECT_EQ (dims[0].quantity, ed::Quantity::Length);
}

TEST (EditDimension, TypedLengthKeepsTheStartAndDirection)
{
    Line l;
    std::string error;
    ASSERT_TRUE (ed::WriteDimension ({ l.seg, ed::Quantity::Length }, 10.0) (l.doc, error)) << error;
    EXPECT_NEAR (l.P (1).x, 6.0, 1e-12);
    EXPECT_NEAR (l.P (1).y, 8.0, 1e-12);
    EXPECT_DOUBLE_EQ (l.P (0).x, 0.0);
    EXPECT_FALSE (ed::WriteDimension ({ l.seg, ed::Quantity::Length }, 0.0) (l.doc, error));
}

TEST (EditDimension, LockedStartMakesTheEndThePivot)
{
    Line l;
    ed::Find (l.doc, l.e)->vertices[1].lockXY = true;
    ed::Find (l.doc, l.e)->vertices[1].lockZ = true;
    std::string error;
    ASSERT_TRUE (ed::WriteDimension ({ l.seg, ed::Quantity::Length }, 10.0) (l.doc, error)) << error;
    EXPECT_NEAR (l.P (0).x, -3.0, 1e-12);
    EXPECT_NEAR (l.P (0).y, -4.0, 1e-12);
    // A slope still reads start to end: +4 % means the start sits lower.
    ASSERT_TRUE (ed::WriteDimension ({ l.seg, ed::Quantity::Slope }, 4.0) (l.doc, error)) << error;
    EXPECT_NEAR (l.P (0).z, 10.0 - 0.4, 1e-12);
    double v = 0;
    ASSERT_TRUE (ed::ReadDimension (l.doc, { l.seg, ed::Quantity::Slope }, v));
    EXPECT_NEAR (v, 4.0, 1e-9);
    ASSERT_TRUE (ed::WriteDimension ({ l.seg, ed::Quantity::Angle }, 0.0) (l.doc, error)) << error;
    EXPECT_NEAR (l.P (0).y, 4.0, 1e-9);
    EXPECT_NEAR (l.P (0).x, -7.0, 1e-9);
    ed::Find (l.doc, l.e)->vertices[0].lockXY = true;
    EXPECT_FALSE (ed::WriteDimension ({ l.seg, ed::Quantity::Length }, 2.0) (l.doc, error));
    EXPECT_NE (error.find ("locked"), std::string::npos);
}

TEST (EditDimension, TypedSlopeMovesOnlyTheFreeEndsHeight)
{
    Line l;
    std::string error;
    ASSERT_TRUE (ed::WriteDimension ({ l.seg, ed::Quantity::Slope }, -2.0) (l.doc, error)) << error;
    EXPECT_NEAR (l.P (1).z, 9.9, 1e-12);
    EXPECT_NEAR (l.P (1).x, 3.0, 1e-12);
}

TEST (EditDimension, VertexElevationAndExtrusionHeight)
{
    Line l;
    const ed::Ref v { l.e, ed::Part::Vertex, ed::Find (l.doc, l.e)->vertices[0].id };
    std::string error;
    ASSERT_TRUE (ed::WriteDimension ({ v, ed::Quantity::Elevation }, 12.0) (l.doc, error));
    EXPECT_DOUBLE_EQ (l.P (0).z, 12.0);
    ed::Find (l.doc, l.e)->vertices[0].lockZ = true;
    EXPECT_FALSE (ed::WriteDimension ({ v, ed::Quantity::Elevation }, 1.0) (l.doc, error));

    const ed::Id box = ed::AddExtrusion (l.doc, { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 } }, 2.0);
    const auto dims = ed::DimensionsFor (l.doc, { box, ed::Part::Face, 1 });
    ASSERT_EQ (dims.size (), 1u);
    ASSERT_TRUE (ed::WriteDimension (dims[0], 3.5) (l.doc, error)) << error;
    EXPECT_DOUBLE_EQ (ed::Find (l.doc, box)->height, 3.5);
    EXPECT_FALSE (ed::WriteDimension (dims[0], -1.0) (l.doc, error));
}

TEST (EditDimension, WidthIsWrittenToTheSegment)
{
    Line l;
    std::string error;
    ASSERT_TRUE (ed::WriteDimension ({ l.seg, ed::Quantity::Width }, 1.8) (l.doc, error));
    double v = 0;
    ASSERT_TRUE (ed::ReadDimension (l.doc, { l.seg, ed::Quantity::Width }, v));
    EXPECT_DOUBLE_EQ (v, 1.8);
    EXPECT_EQ (ed::DimensionsFor (l.doc, l.seg).size (), 4u);
}

TEST (EditDimension, TypingRunsThroughTheSessionLikeADrag)
{
    Line l;
    ed::Session s (l.doc);
    ASSERT_TRUE (s.Apply (ed::WriteDimension ({ l.seg, ed::Quantity::Length }, 2.5)));
    EXPECT_EQ (s.UndoDepth (), 1u);
    ASSERT_TRUE (s.Undo ());
    double v = 0;
    ASSERT_TRUE (ed::ReadDimension (s.Doc (), { l.seg, ed::Quantity::Length }, v));
    EXPECT_DOUBLE_EQ (v, 5.0);
}
