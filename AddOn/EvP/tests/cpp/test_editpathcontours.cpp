#include "Edit/EditPathContours.hpp"
#include "Edit/EditPathEdit.hpp"
#include <gtest/gtest.h>
#include <cmath>

namespace ed = geomsrv::edit;

namespace {
// A 2 m wide path along X from (0, 0) at height 0 to (10, 0) at `rise`, falling across as asked.
ed::Document Ramp (double rise, double crossfall, ed::Fall fall)
{
    ed::Document doc;
    const ed::Id id = ed::AddPath (doc, { { 0, 0, 0 }, { 10, 0, rise } }, 2.0);
    std::string error;
    EXPECT_TRUE (ed::SetPathCrossfall (doc, id, doc.paths[0].sections[0].id, crossfall, fall, error)) << error;
    return doc;
}

const ed::Contour* At (const std::vector<ed::Contour>& lines, double z)
{
    for (const auto& c : lines)
        if (std::abs (c.z - z) < 1.0e-9)
            return &c;
    return nullptr;
}
} // namespace

TEST (EditPathContours, ALevelRampIsCutStraightAcrossAtEachLevel)
{
    // A 1 m rise over 10 m, flat across: each quarter metre is a line straight across the
    // path, where the grade reaches it.
    const auto doc = Ramp (1.0, 0.0, ed::Fall::Crown);
    const auto lines = ed::PathContours (doc.paths, 0.25);
    for (const double z : { 0.25, 0.5, 0.75 }) {
        const ed::Contour* c = At (lines, z);
        ASSERT_NE (c, nullptr) << z;
        EXPECT_EQ (c->path, doc.paths[0].id);
        double lo = 1e9, hi = -1e9;
        for (const auto& p : c->points) {
            EXPECT_NEAR (p.x, z * 10.0, 1.0e-6) << z;
            EXPECT_DOUBLE_EQ (p.z, z);
            lo = std::min (lo, p.y), hi = std::max (hi, p.y);
        }
        EXPECT_NEAR (lo, -1.0, 1.0e-6); // edge to edge, in one line
        EXPECT_NEAR (hi, 1.0, 1.0e-6);
    }
}

TEST (EditPathContours, ACrossfallSkewsTheLines)
{
    // Falling 2 % to the right on a 10 % grade: the right edge, 4 cm lower than the left,
    // reaches a level 0.4 m further on.
    const auto doc = Ramp (1.0, 2.0, ed::Fall::Right);
    const auto lines = ed::PathContours (doc.paths, 0.5);
    const ed::Contour* c = At (lines, 0.5);
    ASSERT_NE (c, nullptr);
    ed::Vec3 left = c->points.front (), right = c->points.back ();
    if (left.y < right.y)
        std::swap (left, right);
    EXPECT_NEAR (left.y, 1.0, 1.0e-6);
    EXPECT_NEAR (right.y, -1.0, 1.0e-6);
    EXPECT_NEAR (right.x - left.x, 0.4, 0.02);
}

TEST (EditPathContours, CrossingPathsShareTheGroundOnce)
{
    // Where two paths cross, a level is traced on the path the ground belongs to -- the one
    // it lies deeper in -- so the crossing is not lined twice.
    ed::Document doc;
    const ed::Id a = ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 2 } }, 2.0);
    const ed::Id b = ed::AddPath (doc, { { 10, -10, 0.5 }, { 10, 10, 1.5 } }, 2.0);
    const auto lines = ed::PathContours (doc.paths, 0.1);
    size_t inside = 0;
    for (const auto& c : lines)
        for (const auto& p : c.points) {
            const double ax = std::abs (p.y), bx = std::abs (p.x - 10.0); // depth across each
            if (ax > 1.0 - 1.0e-6 || bx > 1.0 - 1.0e-6)
                continue; // outside the shared square
            ++inside;
            if (c.path == a)
                EXPECT_LE (ax, bx + 1.0e-6) << p.x << ", " << p.y;
            else if (c.path == b)
                EXPECT_LE (bx, ax + 1.0e-6) << p.x << ", " << p.y;
        }
    EXPECT_GT (inside, 0u);
}

TEST (EditPathContours, TooFineAnIntervalDrawsNone)
{
    const auto doc = Ramp (1.0, 2.0, ed::Fall::Crown);
    EXPECT_TRUE (ed::PathContours (doc.paths, 0.001, 0.5, 400).empty ());
    EXPECT_TRUE (ed::PathContours (doc.paths, 0.0).empty ());
    double lo = 0.0, hi = 0.0;
    ASSERT_TRUE (ed::PathHeightRange (doc.paths, lo, hi));
    EXPECT_NEAR (lo, -0.02, 1.0e-6); // the crown's edges at the start
    EXPECT_NEAR (hi, 1.0, 1.0e-6);
}

TEST (EditPathContours, RoundStepsAndTheirLevels)
{
    EXPECT_DOUBLE_EQ (ed::NiceStep (2.0, 10), 0.2);
    EXPECT_DOUBLE_EQ (ed::NiceStep (3.0, 10), 0.25);
    EXPECT_DOUBLE_EQ (ed::NiceStep (2.4, 5), 0.5);
    EXPECT_DOUBLE_EQ (ed::NiceStep (20.0, 5), 5.0);
    EXPECT_DOUBLE_EQ (ed::NiceStep (0.0, 5), 0.0);
    const auto levels = ed::Levels (110.3, 111.2, 0.25);
    ASSERT_EQ (levels.size (), 3u);
    EXPECT_DOUBLE_EQ (levels.front (), 110.5);
    EXPECT_DOUBLE_EQ (levels.back (), 111.0);
    EXPECT_EQ (ed::StepDecimals (0.25), 2);
    EXPECT_EQ (ed::StepDecimals (0.5), 1);
    EXPECT_EQ (ed::StepDecimals (2.0), 0);
}
