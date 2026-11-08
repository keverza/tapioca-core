#include "ArchViz/SunStudyObstructionGeometry.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace geomsrv::archviz;

namespace {

double FillArea (const GhPreviewBucket& bucket)
{
    double area = 0.0;
    for (size_t i = 0; i + 2 < bucket.meshIndices.size (); i += 3) {
        const auto& a = bucket.meshVertices[bucket.meshIndices[i]];
        const auto& b = bucket.meshVertices[bucket.meshIndices[i + 1]];
        const auto& c = bucket.meshVertices[bucket.meshIndices[i + 2]];
        const double u[3] = { b.x - a.x, b.y - a.y, b.z - a.z }, v[3] = { c.x - a.x, c.y - a.y, c.z - a.z };
        const double n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        area += 0.5 * std::sqrt (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    }
    return area;
}

evp::sunstudy::SolarCutTriangle Tri (double ax, double ay, double bx, double by, double cx, double cy)
{
    evp::sunstudy::SolarCutTriangle t {};
    const double p[3][2] = { { ax, ay }, { bx, by }, { cx, cy } };
    for (int c = 0; c < 3; ++c) {
        t.v[c][0] = p[c][0];
        t.v[c][1] = p[c][1];
        t.v[c][2] = 0.0;
    }
    return t;
}

} // namespace

TEST (SunStudySolarCutPreview, WonCellsBecomeOneRegionWithItsPerimeter)
{
    // A 4 x 4 block of floor cells, 0.5 m apart, with one cell missing inside.
    std::vector<SolarCutWonCell> cells;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            if (i == 1 && j == 2)
                continue;
            SolarCutWonCell cell;
            cell.position[0] = 10.0 + 0.5 * i;
            cell.position[1] = -3.0 + 0.5 * j;
            cells.push_back (cell);
        }
    GhPreviewDrawables drawables;
    AppendSolarCutWonCells (cells, 0.5, drawables);
    // The hole is filled; the shape runs through the cells' outer edges with
    // its four corners chamfered: (16 - 4 * 1/8) cells of 0.25 m2.
    EXPECT_NEAR (FillArea (drawables.depthTested), 15.5 * 0.25, 1.0e-4);
    // Its perimeter only: three segments a side and four chamfers.
    EXPECT_EQ (drawables.depthTested.lineVertices.size () / 6, 16u);
}

TEST (SunStudySolarCutPreview, OutlineSkipsDiagonalsAndTJunctions)
{
    // One square cut face of three triangles, one diagonal split at (1,1).
    evp::sunstudy::SolarCutResult cut;
    cut.caps = { Tri (0, 0, 2, 0, 2, 2), Tri (0, 0, 1, 1, 0, 2), Tri (1, 1, 2, 2, 0, 2) };
    const double point[3] = { 0.0, 0.0, 0.0 };
    GhPreviewDrawables drawables;
    BuildSolarCutGeometry (cut, point, drawables);
    EXPECT_EQ (drawables.depthTested.lineVertices.size () / 6, 4u); // the square's four sides
    EXPECT_FALSE (drawables.xray.meshIndices.empty ());             // the cut face, seen through
    EXPECT_TRUE (drawables.xray.lineVertices.empty ());             // a plain darker pink, no hatch
}

TEST (SunStudySolarCutPreview, CuttingSheetOmitsSectorCreasesAndTriangulationDiagonals)
{
    // Two non-coplanar triangles share the fan's diagonal. It is a sector
    // crease, not another edge of the cutting sheet, and must not be outlined.
    evp::sunstudy::SolarCutResult cut;
    cut.envelope = { Tri (0, 0, 2, 0, 2, 2), Tri (0, 0, 2, 2, 0, 2) };
    cut.envelope[1].v[2][2] = 1.0;
    const double point[3] = { 0.0, 0.0, 0.0 };
    GhPreviewDrawables drawables;
    BuildSolarCutGeometry (cut, point, drawables);
    EXPECT_EQ (drawables.depthTested.lineVertices.size () / 6, 4u);
}

TEST (SunStudySolarCutPreview, WonRegionIsTrimmedToItsSurface)
{
    // Two 3 x 3 blocks of won cells one empty column apart. Alone, closing
    // joins them across the gap; given the study's cells -- no cell in that
    // column, the surface has an edge there -- they stay two regions.
    std::vector<SolarCutWonCell> cells;
    for (int i = 0; i < 7; ++i)
        for (int j = 0; j < 3; ++j) {
            if (i == 3)
                continue;
            SolarCutWonCell cell;
            cell.position[0] = 0.5 * i;
            cell.position[1] = 0.5 * j;
            cells.push_back (cell);
        }
    GhPreviewDrawables joined, trimmed;
    AppendSolarCutWonCells (cells, 0.5, joined);
    AppendSolarCutWonCells (cells, 0.5, trimmed, &cells);
    EXPECT_GT (FillArea (joined.depthTested), FillArea (trimmed.depthTested) + 0.2);
    EXPECT_NEAR (FillArea (trimmed.depthTested), 2.0 * 8.5 * 0.25, 1.0e-4); // each block, corners chamfered
}
