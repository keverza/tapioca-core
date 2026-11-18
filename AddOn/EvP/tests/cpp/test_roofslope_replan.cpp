// Preservation-first edits on the existing rectangle preset: an edit changes only the
// geometry it is connected to and never silently moves or drops a locked control.
#include "ArchViz/RoofSlope.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <string>
#include <vector>

namespace rs = geomsrv::archviz::roofslope;
namespace {
rs::Data Plan (std::vector<rs::Ring> rings, double top = 5, double cap = 200)
{
    rs::Data d;
    std::string error;
    EXPECT_TRUE (rs::Plan (rings, top, 2, cap, d, error)) << error;
    return d;
}
double Area (const rs::Data& d)
{
    double sum = 0;
    for (const auto& t : d.triangles)
        sum += t.area;
    return sum;
}
int CellWithSide (const rs::Data& d, int side, double value, double vMin, double vMax)
{
    for (int i = 0; i < int (d.cells.size ()); ++i) {
        const auto& b = d.cells[i].box;
        if (std::abs (b[side] - value) < 1e-6 && b[1] >= vMin - 1e-6 && b[3] <= vMax + 1e-6)
            return i;
    }
    return -1;
}
} // namespace

TEST (RoofSlopeReplan, DividerMoveStaysOnItsConnectedChain)
{
    // Two disconnected wings whose dividers happen to share x = 10.
    auto d =
        Plan ({ { { 0, 0 }, { 20, 0 }, { 20, 10 }, { 0, 10 } }, { { 0, 20 }, { 20, 20 }, { 20, 30 }, { 0, 30 } } });
    ASSERT_NEAR (d.bearing, 0, 1e-9);
    ASSERT_EQ (d.cells.size (), 4u);
    const int lower = CellWithSide (d, 2, 10, 0, 10);
    ASSERT_GE (lower, 0);
    std::vector<std::array<double, 4>> upper;
    for (const auto& c : d.cells)
        if (c.box[1] >= 20 - 1e-6)
            upper.push_back (c.box);
    ASSERT_EQ (upper.size (), 2u);
    std::string error;
    ASSERT_TRUE (rs::MoveDivider (d, lower, 2, 10.5, error)) << error;
    EXPECT_NEAR (Area (d), 400, 1e-4);
    std::vector<std::array<double, 4>> after;
    for (const auto& c : d.cells)
        if (c.box[1] >= 20 - 1e-6)
            after.push_back (c.box);
    EXPECT_EQ (after, upper) << "the disconnected wing's divider must not move";
    EXPECT_GE (CellWithSide (d, 2, 10.5, 0, 10), 0) << "the picked divider moved";
}
TEST (RoofSlopeReplan, DividerMoveKeepsAnXyLockedDrainOutsideItsChain)
{
    auto d =
        Plan ({ { { 0, 0 }, { 20, 0 }, { 20, 10 }, { 0, 10 } }, { { 0, 20 }, { 20, 20 }, { 20, 30 }, { 0, 30 } } });
    ASSERT_EQ (d.cells.size (), 4u);
    const int lower = CellWithSide (d, 2, 10, 0, 10);
    const int upper = CellWithSide (d, 2, 10, 20, 30);
    ASSERT_GE (lower, 0);
    ASSERT_GE (upper, 0);
    d.nodes[d.cells[upper].drain].lockXY = true;
    const auto locked = d.nodes[d.cells[upper].drain].xy;
    std::string error;
    ASSERT_TRUE (rs::MoveDivider (d, lower, 2, 10.5, error)) << error;
    bool kept = false;
    for (const auto& n : d.nodes)
        kept = kept || (n.lockXY && std::abs (n.xy.x - locked.x) < 1e-9 && std::abs (n.xy.y - locked.y) < 1e-9);
    EXPECT_TRUE (kept);
}
