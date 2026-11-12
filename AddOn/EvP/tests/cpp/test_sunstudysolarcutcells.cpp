#include "ArchViz/SunStudyPreview.hpp"
#include "SunStudy/SunStudySolarCutCells.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <vector>

using namespace evp::sunstudy;

namespace {
SolarCutTriangle Tri (std::array<double, 3> a, std::array<double, 3> b, std::array<double, 3> c)
{
    SolarCutTriangle t;
    for (int i = 0; i < 3; ++i) {
        t.v[0][i] = a[i];
        t.v[1][i] = b[i];
        t.v[2][i] = c[i];
    }
    return t;
}

// A 0.5 m cell at column i, row j of a floor lattice at the origin.
SolarCutCell Cell (int i, int j, uint32_t sample, float before, float after, uint64_t surface = 7)
{
    SolarCutCell cell;
    cell.surface = surface;
    cell.sample = sample;
    cell.before = before;
    cell.after = after;
    const double s = 0.5, du[4] = { 0, 1, 1, 0 }, dv[4] = { 0, 0, 1, 1 };
    for (int k = 0; k < 4; ++k) {
        cell.corners[k][0] = (i + du[k]) * s;
        cell.corners[k][1] = (j + dv[k]) * s;
        cell.corners[k][2] = 0.0;
    }
    return cell;
}

double Area (const std::vector<SolarCutTriangle>& triangles)
{
    double area = 0.0;
    for (const auto& t : triangles) {
        const double ux = t.v[1][0] - t.v[0][0], uy = t.v[1][1] - t.v[0][1];
        const double vx = t.v[2][0] - t.v[0][0], vy = t.v[2][1] - t.v[0][1];
        area += 0.5 * std::abs (ux * vy - uy * vx);
    }
    return area;
}
} // namespace

// The cells the cut changes are drawn in their after-cut hours, grouped by
// value, clipped to the surface they lie on, and outlined ONCE around the
// merged region -- not cell by cell.
TEST (SunStudySolarCutCells, CellsAreGroupedByHoursClippedToTheSurfaceAndOutlinedOnce)
{
    // A 2 m x 0.75 m floor: the second row of cells hangs half off it.
    std::map<uint64_t, std::vector<SolarCutTriangle>> surfaces;
    surfaces[7] = { Tri ({ 0, 0, 0 }, { 2, 0, 0 }, { 2, 0.75, 0 }), Tri ({ 0, 0, 0 }, { 2, 0.75, 0 }, { 0, 0.75, 0 }) };
    const std::vector<SolarCutCell> cells = {
        Cell (0, 0, 10, 2.0f, 3.0f), Cell (1, 0, 11, 2.0f, 3.0f), Cell (2, 0, 12, 2.0f, 4.0f),
        Cell (3, 0, 13, 2.5f, 4.0f), Cell (0, 1, 14, 1.0f, 3.0f), // half on the floor
    };
    SolarCutCellsResult result;
    ASSERT_TRUE (BuildSolarCutCells (cells, surfaces, result)) << result.error;
    EXPECT_EQ (result.cells, 5u);
    EXPECT_EQ (result.unplaced, 0u);
    ASSERT_EQ (result.groups.size (), 2u);
    EXPECT_FLOAT_EQ (result.groups[0].after, 3.0f);
    EXPECT_FLOAT_EQ (result.groups[1].after, 4.0f);
    EXPECT_NEAR (Area (result.groups[0].triangles), 0.5 + 0.125, 1.0e-9);
    EXPECT_NEAR (Area (result.groups[1].triangles), 0.5, 1.0e-9);
    EXPECT_NEAR (result.area, 1.125, 1.0e-9);
    // One outline around the merged L: no edge between neighbouring cells.
    double perimeter = 0.0;
    for (const auto& e : result.outline)
        perimeter += std::hypot (e[3] - e[0], e[4] - e[1]);
    EXPECT_NEAR (perimeter, 2.0 + 0.5 + 1.5 + 0.25 + 0.5 + 0.75, 1.0e-6);
    EXPECT_FLOAT_EQ (result.mostGained, 2.0f);
    float before = 0.0f, after = 0.0f;
    ASSERT_TRUE (result.Read (13, before, after));
    EXPECT_FLOAT_EQ (before, 2.5f);
    EXPECT_FLOAT_EQ (after, 4.0f);
    EXPECT_FALSE (result.Read (99, before, after));
}

// A changed cell with no surface to draw on is counted, not invented.
TEST (SunStudySolarCutCells, ACellWithoutItsSurfaceIsCountedNotDrawn)
{
    SolarCutCellsResult result;
    ASSERT_TRUE (BuildSolarCutCells ({ Cell (0, 0, 1, 1.0f, 2.0f, 3) }, {}, result)) << result.error;
    EXPECT_EQ (result.cells, 0u);
    EXPECT_EQ (result.unplaced, 1u);
    EXPECT_TRUE (result.groups.empty ());
    float before = 0.0f, after = 0.0f;
    EXPECT_TRUE (result.Read (1, before, after)); // the hover still reads it
}

// The cells are coloured as the tint colours the map: stepped band centres,
// the smooth ramp, and the low-sun swatch below its threshold.
TEST (SunStudySolarCutCells, HoursColourMatchesTheTintsRamp)
{
    using geomsrv::archviz::SunStudyHoursRgba;
    using geomsrv::archviz::SunStudyRampColour;
    const auto stops = geomsrv::archviz::SunStudyRampStops (geomsrv::archviz::kSunRampDefaultPreset);
    const float edges[11] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10 };
    EXPECT_EQ (SunStudyHoursRgba (stops, 3.25f, 10, edges, -1.0f, 0x1F5FD1FFu), SunStudyRampColour (stops, 3.5f));
    EXPECT_EQ (SunStudyHoursRgba (stops, 3.25f, 0, nullptr, -1.0f, 0x1F5FD1FFu), SunStudyRampColour (stops, 3.25f));
    EXPECT_EQ (SunStudyHoursRgba (stops, 1.0f, 10, edges, 2.5f, 0x1F5FD100u), 0x1F5FD1FFu);
}
