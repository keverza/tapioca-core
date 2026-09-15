// Tests for SunStudy/SurfacePatch — the analysis domain above the triangles.
//
// ⚠️ THE FIXTURE THAT MATTERS IS THE FIRST ONE: a rectangle Archicad handed over
// as two triangles must be ONE analysis domain with ONE lattice. Today it is two
// domains with two lattices and a diagonal seam down the middle of a surface
// that has no seam in it -- and the renderer is faithfully showing what the
// sampler defines, so the fix has to be here.

#include <cmath>
#include <set>
#include <vector>

#include "SunStudy/SurfacePatch.hpp"
#include "gtest/gtest.h"

using namespace evp::sunstudy;

namespace {

struct Mesh {
    std::vector<double> vertices;
    std::vector<uint32_t> triangles;
    std::vector<uint32_t> groups;
};

// A w x h rectangle in z = 0, split on the 0-2 diagonal: the exact case.
Mesh Rectangle (double w, double h)
{
    Mesh mesh;
    mesh.vertices = { 0, 0, 0, w, 0, 0, w, h, 0, 0, h, 0 };
    mesh.triangles = { 0, 1, 2, 0, 2, 3 };
    mesh.groups = { 0, 0 };
    return mesh;
}

// The same rectangle chopped into `strips` columns, each two triangles: what a
// real extraction of a wall with openings nearby tends to look like.
Mesh StrippedRectangle (double w, double h, int strips)
{
    Mesh mesh;
    for (int i = 0; i <= strips; ++i) {
        const double x = w * i / strips;
        mesh.vertices.insert (mesh.vertices.end (), { x, 0.0, 0.0 });
        mesh.vertices.insert (mesh.vertices.end (), { x, h, 0.0 });
    }
    for (int i = 0; i < strips; ++i) {
        const uint32_t a = uint32_t (i * 2);
        const uint32_t b = uint32_t (i * 2 + 1);
        const uint32_t c = uint32_t (i * 2 + 2);
        const uint32_t d = uint32_t (i * 2 + 3);
        mesh.triangles.insert (mesh.triangles.end (), { a, c, d });
        mesh.triangles.insert (mesh.triangles.end (), { a, d, b });
        mesh.groups.insert (mesh.groups.end (), { 0, 0 });
    }
    return mesh;
}

size_t PatchesOf (const Mesh& mesh, std::vector<SurfacePatch>& out)
{
    out = BuildSurfacePatches (mesh.vertices.data (), mesh.vertices.size () / 3, mesh.triangles.data (),
                               mesh.triangles.size () / 3, mesh.groups.data ());
    return out.size ();
}

} // namespace

// ---------------------------------------------------------------------------
// the proof the task asked for
// ---------------------------------------------------------------------------

TEST (SurfacePatch, TwoCoplanarTrianglesAreOneSurface)
{
    const Mesh mesh = Rectangle (4.0, 3.0);
    std::vector<SurfacePatch> patches;
    ASSERT_EQ (PatchesOf (mesh, patches), 1u);
    EXPECT_EQ (patches[0].triangles.size (), 2u);
    EXPECT_NEAR (patches[0].area, 12.0, 1e-9);
    // The frame spans the rectangle, not one of its triangles.
    EXPECT_NEAR (patches[0].uExtent, 4.0, 1e-9);
    EXPECT_NEAR (patches[0].vExtent, 3.0, 1e-9);
}

TEST (SurfacePatch, TwentyCoplanarTrianglesAreStillOneSurface)
{
    const Mesh mesh = StrippedRectangle (10.0, 2.0, 10); // 20 triangles
    ASSERT_EQ (mesh.triangles.size () / 3, 20u);
    std::vector<SurfacePatch> patches;
    ASSERT_EQ (PatchesOf (mesh, patches), 1u);
    EXPECT_EQ (patches[0].triangles.size (), 20u);
    EXPECT_NEAR (patches[0].area, 20.0, 1e-9);
}

TEST (SurfacePatch, OneLatticeCoversTheWholeRectangleWithNoDiagonalSeam)
{
    // ⚠️ THIS IS THE ARTEFACT, STATED AS A NUMBER. Two triangle-local grids give
    // the two halves of a rectangle different origins and different lattices, so
    // the cells do not line up across the diagonal. One patch grid produces
    // exactly the cells a 4x3 rectangle at 1 m should have: twelve of them, each
    // 1 m x 1 m, in one lattice.
    const Mesh mesh = Rectangle (4.0, 3.0);
    std::vector<SurfacePatch> patches;
    ASSERT_EQ (PatchesOf (mesh, patches), 1u);

    const PatchGrid grid = BuildPatchGrid (patches[0], mesh.vertices.data (), mesh.triangles.data (), 1.0);
    EXPECT_EQ (grid.columns, 4u);
    EXPECT_EQ (grid.rows, 3u);
    EXPECT_EQ (grid.cells.size (), 12u);
    EXPECT_FALSE (grid.fromCentroid);

    // Every cell is interior and square: no cell is clipped, because the patch
    // IS the rectangle. A per-triangle grid cannot say this -- its diagonal cells
    // are all partial.
    std::set<std::pair<uint32_t, uint32_t>> seen;
    for (const PatchCell& cell : grid.cells) {
        EXPECT_TRUE (cell.interior) << "cell " << cell.column << "," << cell.row << " was clipped";
        EXPECT_NEAR (cell.area, 1.0, 1e-9);
        EXPECT_NEAR (cell.centre[2], 0.0, 1e-9);
        EXPECT_TRUE (seen.insert ({ cell.column, cell.row }).second) << "duplicate cell";
    }
    // And the twelve are the twelve, once each.
    EXPECT_EQ (seen.size (), 12u);
}

TEST (SurfacePatch, TheLatticeIsIndependentOfHowTheRectangleWasTriangulated)
{
    // ⚠️ THE SEMANTIC CLAIM, TESTED DIRECTLY: the same surface cut into twenty
    // triangles instead of two must measure the same places. If this fails, the
    // study still depends on the extractor's triangulation and nothing has been
    // fixed.
    const Mesh two = Rectangle (10.0, 2.0);
    const Mesh twenty = StrippedRectangle (10.0, 2.0, 10);

    std::vector<SurfacePatch> a, b;
    ASSERT_EQ (PatchesOf (two, a), 1u);
    ASSERT_EQ (PatchesOf (twenty, b), 1u);

    const PatchGrid gridA = BuildPatchGrid (a[0], two.vertices.data (), two.triangles.data (), 1.0);
    const PatchGrid gridB = BuildPatchGrid (b[0], twenty.vertices.data (), twenty.triangles.data (), 1.0);

    EXPECT_EQ (gridA.columns, gridB.columns);
    EXPECT_EQ (gridA.rows, gridB.rows);
    ASSERT_EQ (gridA.cells.size (), gridB.cells.size ());
    EXPECT_EQ (gridA.cells.size (), 20u);

    for (size_t i = 0; i < gridA.cells.size (); ++i) {
        for (int axis = 0; axis < 3; ++axis)
            EXPECT_NEAR (gridA.cells[i].centre[axis], gridB.cells[i].centre[axis], 1e-9)
                << "cell " << i << " axis " << axis;
    }
}

// ---------------------------------------------------------------------------
// what must NOT merge
// ---------------------------------------------------------------------------

TEST (SurfacePatch, PerpendicularRectanglesAreTwoSurfaces)
{
    Mesh mesh;
    // A floor in z = 0 and a wall in y = 0, sharing the edge along x.
    mesh.vertices = { 0, 0, 0, 4, 0, 0, 4, 3, 0, 0, 3, 0, // floor
                      0, 0, 3, 4, 0, 3 };                 // wall top edge
    mesh.triangles = { 0, 1, 2, 0, 2, 3, 0, 1, 5, 0, 5, 4 };
    mesh.groups = { 0, 0, 0, 0 };
    std::vector<SurfacePatch> patches;
    EXPECT_EQ (PatchesOf (mesh, patches), 2u);
}

TEST (SurfacePatch, CoplanarButDisconnectedPiecesAreTwoSurfaces)
{
    // ⚠️ EDGE-CONNECTED, NOT MERELY COPLANAR. Two slabs in the same plane at
    // opposite ends of a site are two surfaces; gridding them as one would put a
    // single lattice across the gap and hand the empty middle to neither.
    Mesh mesh;
    mesh.vertices = { 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 10, 0, 0, 11, 0, 0, 11, 1, 0, 10, 1, 0 };
    mesh.triangles = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };
    mesh.groups = { 0, 0, 0, 0 };
    std::vector<SurfacePatch> patches;
    EXPECT_EQ (PatchesOf (mesh, patches), 2u);
}

TEST (SurfacePatch, TwoElementsNeverShareAPatchEvenWhenFlush)
{
    // Same geometry as the connected rectangle, different group ids.
    Mesh mesh = Rectangle (4.0, 3.0);
    mesh.groups = { 0, 1 };
    std::vector<SurfacePatch> patches;
    ASSERT_EQ (PatchesOf (mesh, patches), 2u);
    EXPECT_NE (patches[0].group, patches[1].group);
}

TEST (SurfacePatch, TheTwoSidesOfASlabAreTwoSurfaces)
{
    // Antiparallel normals in the same plane: the front and back of a wall.
    Mesh mesh;
    mesh.vertices = { 0, 0, 0, 4, 0, 0, 4, 3, 0, 0, 3, 0 };
    mesh.triangles = { 0, 1, 2, 0, 3, 2 }; // the second wound the other way
    mesh.groups = { 0, 0 };
    std::vector<SurfacePatch> patches;
    EXPECT_EQ (PatchesOf (mesh, patches), 2u);
}

// ---------------------------------------------------------------------------
// coverage and clipping
// ---------------------------------------------------------------------------

namespace {

double TotalCellArea (const PatchGrid& grid)
{
    double total = 0.0;
    for (const PatchCell& cell : grid.cells)
        total += cell.area;
    return total;
}

} // namespace

TEST (SurfacePatch, TheCellAreasSumToThePatchAreaAtEverySpacing)
{
    // ⚠️ THE INVARIANT THE WHOLE CLIPPER IS CHECKABLE BY. A clipper that drops a
    // piece, double-counts a shared edge or mis-orients a triangle breaks this
    // immediately -- and none of those would have shown in a picture, which is
    // precisely why the artefact they cause (an area-weighted mean that is
    // quietly wrong) went unnoticed in the engine this replaces.
    const Mesh mesh = Rectangle (4.0, 3.0);
    std::vector<SurfacePatch> patches;
    ASSERT_EQ (PatchesOf (mesh, patches), 1u);

    for (const double spacing : { 0.35, 0.5, 0.7, 1.0, 1.3, 2.0 }) {
        const PatchGrid grid = BuildPatchGrid (patches[0], mesh.vertices.data (), mesh.triangles.data (), spacing);
        EXPECT_NEAR (TotalCellArea (grid), patches[0].area, 1e-9) << "spacing " << spacing;
    }
}

TEST (SurfacePatch, ATriangleClipsToExactlyHalfTheSquare)
{
    // A right triangle over a 4x4 lattice: the clipped cells along the
    // hypotenuse must add up with the whole ones to the triangle's own area, and
    // NOT to the bounding square's.
    Mesh mesh;
    mesh.vertices = { 0, 0, 0, 4, 0, 0, 0, 4, 0 };
    mesh.triangles = { 0, 1, 2 };
    mesh.groups = { 0 };
    std::vector<SurfacePatch> patches;
    ASSERT_EQ (PatchesOf (mesh, patches), 1u);
    EXPECT_NEAR (patches[0].area, 8.0, 1e-9);

    const PatchGrid grid = BuildPatchGrid (patches[0], mesh.vertices.data (), mesh.triangles.data (), 1.0);
    EXPECT_NEAR (TotalCellArea (grid), 8.0, 1e-9);

    // The cell in the far corner is entirely outside and must not exist at all.
    for (const PatchCell& cell : grid.cells)
        EXPECT_FALSE (cell.column == 3 && cell.row == 3) << "an uncovered cell was emitted";

    // A clipped cell's sample is its covered part's centroid, so it lies inside
    // the triangle -- u + v <= 4 -- which the middle of the square does not.
    for (const PatchCell& cell : grid.cells) {
        if (cell.interior)
            continue;
        EXPECT_LE (cell.centre[0] + cell.centre[1], 4.0 + 1e-9) << "a clipped cell sampled outside its own surface";
    }
}

TEST (SurfacePatch, AnLShapedPatchLeavesItsMissingQuadrantEmpty)
{
    // Two rectangles meeting in an L, coplanar and edge-connected: one patch,
    // and the missing quadrant must contribute nothing.
    Mesh mesh;
    mesh.vertices = { 0, 0, 0, 4, 0, 0, 4, 2, 0, 0, 2, 0, 2, 4, 0, 0, 4, 0 };
    // bottom bar 0-1-2-3, upper-left bar 3-2'? -- use explicit triangles:
    mesh.triangles = { 0, 1, 2, 0, 2, 3, 3, 2, 4, 3, 4, 5 };
    mesh.groups = { 0, 0, 0, 0 };
    std::vector<SurfacePatch> patches;
    ASSERT_EQ (PatchesOf (mesh, patches), 1u);

    const PatchGrid grid = BuildPatchGrid (patches[0], mesh.vertices.data (), mesh.triangles.data (), 1.0);
    EXPECT_NEAR (TotalCellArea (grid), patches[0].area, 1e-9);
    // ⚠️ THE POINT OF THE FIXTURE: the concave corner is not filled in. A clipper
    // that worked from a bounding rectangle, or from a convex hull, would cover
    // the empty quadrant and report sunlight on a surface that is not there.
    EXPECT_LT (TotalCellArea (grid), 4.0 * 4.0);
}

TEST (SurfacePatch, TheGridIsIdenticalOnARotatedPlane)
{
    // ⚠️ THE LATTICE IS THE PATCH'S OWN, so a wall standing up must measure the
    // same as the same rectangle lying down. A grid that leaked a world axis in
    // would give the two different cells -- and would lay a diagonal lattice
    // across every facade that is not axis-aligned.
    const Mesh flat = Rectangle (4.0, 3.0);
    Mesh upright; // the same rectangle rotated into the x-z plane
    upright.vertices = { 0, 0, 0, 4, 0, 0, 4, 0, 3, 0, 0, 3 };
    upright.triangles = { 0, 1, 2, 0, 2, 3 };
    upright.groups = { 0, 0 };

    std::vector<SurfacePatch> a, b;
    ASSERT_EQ (PatchesOf (flat, a), 1u);
    ASSERT_EQ (PatchesOf (upright, b), 1u);

    const PatchGrid gridA = BuildPatchGrid (a[0], flat.vertices.data (), flat.triangles.data (), 1.0);
    const PatchGrid gridB = BuildPatchGrid (b[0], upright.vertices.data (), upright.triangles.data (), 1.0);
    ASSERT_EQ (gridA.cells.size (), gridB.cells.size ());
    for (size_t i = 0; i < gridA.cells.size (); ++i) {
        EXPECT_EQ (gridA.cells[i].column, gridB.cells[i].column);
        EXPECT_EQ (gridA.cells[i].row, gridB.cells[i].row);
        EXPECT_NEAR (gridA.cells[i].area, gridB.cells[i].area, 1e-9);
    }
}

TEST (SurfacePatch, TwoAndTwentyTrianglesAgreeOnAreasAndSamplesNotJustCentres)
{
    // The strongest regression in the file, now asserting everything a study
    // would actually measure -- cell count, areas, sample points and the total.
    const Mesh two = Rectangle (10.0, 2.0);
    const Mesh twenty = StrippedRectangle (10.0, 2.0, 10);

    std::vector<SurfacePatch> a, b;
    ASSERT_EQ (PatchesOf (two, a), 1u);
    ASSERT_EQ (PatchesOf (twenty, b), 1u);

    // ⚠️ A SPACING THAT DOES NOT DIVIDE THE RECTANGLE, so the last column is
    // clipped. An evenly dividing spacing would let a broken clipper pass by
    // never exercising a partial cell.
    const double spacing = 0.7;
    const PatchGrid gridA = BuildPatchGrid (a[0], two.vertices.data (), two.triangles.data (), spacing);
    const PatchGrid gridB = BuildPatchGrid (b[0], twenty.vertices.data (), twenty.triangles.data (), spacing);

    ASSERT_EQ (gridA.cells.size (), gridB.cells.size ());
    for (size_t i = 0; i < gridA.cells.size (); ++i) {
        EXPECT_EQ (gridA.cells[i].column, gridB.cells[i].column);
        EXPECT_EQ (gridA.cells[i].row, gridB.cells[i].row);
        EXPECT_NEAR (gridA.cells[i].area, gridB.cells[i].area, 1e-9) << "cell " << i;
        for (int axis = 0; axis < 3; ++axis)
            EXPECT_NEAR (gridA.cells[i].centre[axis], gridB.cells[i].centre[axis], 1e-9) << "cell " << i;
    }
    EXPECT_NEAR (TotalCellArea (gridA), TotalCellArea (gridB), 1e-9);
    EXPECT_NEAR (TotalCellArea (gridA), 20.0, 1e-9);
}

TEST (SurfacePatch, ATriangleClipsItsBoundaryCellsAndKeepsItsInteriorSquare)
{
    // A right triangle is the honest case for clipping: the hypotenuse cuts
    // cells, and those must be MARKED rather than silently carrying spacing².
    Mesh mesh;
    mesh.vertices = { 0, 0, 0, 4, 0, 0, 0, 4, 0 };
    mesh.triangles = { 0, 1, 2 };
    mesh.groups = { 0 };
    std::vector<SurfacePatch> patches;
    ASSERT_EQ (PatchesOf (mesh, patches), 1u);

    const PatchGrid grid = BuildPatchGrid (patches[0], mesh.vertices.data (), mesh.triangles.data (), 1.0);
    ASSERT_FALSE (grid.cells.empty ());
    size_t interior = 0;
    size_t clipped = 0;
    for (const PatchCell& cell : grid.cells)
        (cell.interior ? interior : clipped)++;
    EXPECT_GT (interior, 0u) << "a 4x4 right triangle at 1 m has whole cells in its corner";
    EXPECT_GT (clipped, 0u) << "and partial ones along the hypotenuse";
}

TEST (SurfacePatch, ASurfaceBelowTheGridStillGetsASample)
{
    // ⚠️ COVERAGE IS A CORRECTNESS PROPERTY. A mullion smaller than the spacing
    // dropped from the study leaves every total looking plausible.
    //
    // ⚠️ AND THE CLIPPER NOW SATISFIES IT WITHOUT THE CENTROID FALLBACK. A patch
    // ten centimetres across still COVERS part of the two-metre cell it sits in,
    // so it emits one cell carrying its real area and its true centroid -- which
    // is strictly better than the fallback's estimate. The fallback survives for
    // the pathological case where no cell registers any coverage at all; what is
    // asserted here is the GUARANTEE, not which branch met it.
    const Mesh mesh = Rectangle (0.1, 0.1);
    std::vector<SurfacePatch> patches;
    ASSERT_EQ (PatchesOf (mesh, patches), 1u);
    const PatchGrid grid = BuildPatchGrid (patches[0], mesh.vertices.data (), mesh.triangles.data (), 2.0);
    ASSERT_EQ (grid.cells.size (), 1u);
    // The patch's real area, never spacing².
    EXPECT_NEAR (grid.cells[0].area, 0.01, 1e-12);
    EXPECT_FALSE (grid.cells[0].interior);
    // And the sample sits ON the surface, not in the middle of the empty cell.
    EXPECT_NEAR (grid.cells[0].centre[0], 0.05, 1e-9);
    EXPECT_NEAR (grid.cells[0].centre[1], 0.05, 1e-9);
}

TEST (SurfacePatch, DegenerateTrianglesAreDroppedRatherThanRotatingAPatchsPlane)
{
    Mesh mesh = Rectangle (4.0, 3.0);
    // A zero-area sliver sharing an edge with the rectangle.
    mesh.vertices.insert (mesh.vertices.end (), { 2.0, 0.0, 0.0 });
    mesh.triangles.insert (mesh.triangles.end (), { 0, 1, 4 });
    mesh.groups.push_back (0);
    std::vector<SurfacePatch> patches;
    ASSERT_EQ (PatchesOf (mesh, patches), 1u);
    EXPECT_EQ (patches[0].triangles.size (), 2u) << "the sliver must not have joined";
}

TEST (SurfacePatch, PatchesAreDeterministic)
{
    // ⚠️ THE ATLAS PACKS BY PATCH. A patch set that came out in a different order
    // between runs would repack the atlas and silently invalidate every texture
    // coordinate already handed out.
    const Mesh mesh = StrippedRectangle (10.0, 2.0, 10);
    std::vector<SurfacePatch> first, second;
    PatchesOf (mesh, first);
    PatchesOf (mesh, second);
    ASSERT_EQ (first.size (), second.size ());
    for (size_t i = 0; i < first.size (); ++i) {
        EXPECT_EQ (first[i].triangles, second[i].triangles);
        for (int axis = 0; axis < 3; ++axis) {
            EXPECT_NEAR (first[i].origin[axis], second[i].origin[axis], 1e-12);
            EXPECT_NEAR (first[i].uAxis[axis], second[i].uAxis[axis], 1e-12);
        }
    }
}
