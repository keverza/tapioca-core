// ArchViz/SectionCaps -- the section plane's cap fill and section line (issue #14 P2).
//
// The cap is judged by AREA, not by triangle count: a count says nothing about whether the right
// region was covered, while a box's cross-section has one right answer per axis. The acceptance's
// "caps handle holes and fail transparently on unsuitable meshes" is two of the cases below.

#include "ArchViz/SectionCaps.hpp"

#include "ArchViz/MatrixMath.hpp"

#include <gtest/gtest.h>

#include <memory>

using namespace geomsrv::archviz;

namespace {

// An axis-aligned box; `skipX1` leaves the +x face off, so the mesh is open there.
void AddBox (SectionMesh& m, float x0, float y0, float z0, float x1, float y1, float z1, bool skipX1 = false)
{
    const uint32_t base = uint32_t (m.positions.size () / 3);
    for (int k = 0; k < 2; ++k)
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i)
                m.positions.insert (m.positions.end (), { i ? x1 : x0, j ? y1 : y0, k ? z1 : z0 });
    const uint32_t quads[6][4] = { { 0, 2, 3, 1 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 },
                                   { 2, 6, 7, 3 }, { 0, 4, 6, 2 }, { 1, 3, 7, 5 } };
    for (int f = 0; f < 6; ++f) {
        if (skipX1 && f == 5)
            continue;
        const uint32_t* q = quads[f];
        m.indices.insert (m.indices.end (),
                          { base + q[0], base + q[1], base + q[2], base + q[0], base + q[2], base + q[3] });
    }
    const float lo[3] = { x0, y0, z0 }, hi[3] = { x1, y1, z1 };
    const bool first = base == 0;
    for (int a = 0; a < 3; ++a) {
        m.boundsMin[a] = first ? lo[a] : std::min (m.boundsMin[a], lo[a]);
        m.boundsMax[a] = first ? hi[a] : std::max (m.boundsMax[a], hi[a]);
    }
}

SectionCapResult Cut (const SectionMesh& mesh, SectionAxis axis, float coordinate, bool flipped = false)
{
    SectionCapRequest request;
    request.axis = axis;
    request.coordinate = coordinate;
    request.flipped = flipped;
    request.meshes.push_back (std::make_shared<SectionMesh> (mesh));
    return BuildSectionCaps (request);
}

} // namespace

TEST (SectionCaps, BoxCrossSectionOnEveryAxis)
{
    SectionMesh box;
    AddBox (box, 0.0f, 0.0f, 0.0f, 2.0f, 3.0f, 4.0f);
    const struct {
        SectionAxis axis;
        float at;
        double area;
    } cases[] = { { SectionAxis::Z, 1.0f, 6.0 }, { SectionAxis::X, 1.0f, 12.0 }, { SectionAxis::Y, 1.5f, 8.0 } };
    for (const auto& c : cases) {
        const SectionCapResult r = Cut (box, c.axis, c.at);
        EXPECT_EQ (r.counts.elementsCut, 1u) << SectionAxisName (c.axis);
        EXPECT_EQ (r.counts.closedLoops, 1u) << SectionAxisName (c.axis);
        EXPECT_EQ (r.counts.openChains, 0u) << SectionAxisName (c.axis);
        EXPECT_EQ (r.counts.fillFailures, 0u) << SectionAxisName (c.axis);
        EXPECT_NEAR (r.counts.areaM2, c.area, 1.0e-3) << SectionAxisName (c.axis);
        ASSERT_FALSE (r.poche.vertices.empty ());
        ASSERT_FALSE (r.line.vertices.empty ());
        // Poche plus line cover the cut exactly: the line is inside, not centred on the edge.
        EXPECT_NEAR (SectionCapMeshArea (r.poche) + SectionCapMeshArea (r.line), c.area, 1.0e-3);
        // In the plane's frame the cap lies at z = the cut.
        for (const StorySliceFillVertex& v : r.poche.vertices)
            EXPECT_NEAR (v.z, c.at, 1.0e-3f);
    }
}

TEST (SectionCaps, NestedSolidsLeaveAHole)
{
    SectionMesh tube;
    AddBox (tube, 0.0f, 0.0f, 0.0f, 4.0f, 4.0f, 3.0f);
    AddBox (tube, 1.0f, 1.0f, 0.0f, 3.0f, 3.0f, 3.0f); // inner shell: even-odd makes it a hole
    const SectionCapResult r = Cut (tube, SectionAxis::Z, 1.5f);
    EXPECT_EQ (r.counts.closedLoops, 2u);
    EXPECT_NEAR (r.counts.areaM2, 16.0 - 4.0, 1.0e-3);
}

TEST (SectionCaps, OpenMeshIsOutlinedButNeverFilled)
{
    SectionMesh open;
    AddBox (open, 0.0f, 0.0f, 0.0f, 2.0f, 2.0f, 2.0f, /*skipX1*/ true);
    const SectionCapResult r = Cut (open, SectionAxis::Z, 1.0f);
    EXPECT_EQ (r.counts.elementsCut, 1u);
    EXPECT_EQ (r.counts.closedLoops, 0u);
    EXPECT_EQ (r.counts.openChains, 1u);
    EXPECT_NEAR (r.counts.areaM2, 0.0, 1.0e-9);
    EXPECT_TRUE (r.poche.vertices.empty ());
    EXPECT_FALSE (r.line.vertices.empty ()); // shown, not hidden
}

TEST (SectionCaps, AMissedElementIsNotCut)
{
    SectionMesh box;
    AddBox (box, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
    const SectionCapResult r = Cut (box, SectionAxis::Z, 5.0f);
    EXPECT_EQ (r.counts.elementsCut, 0u);
    EXPECT_TRUE (r.line.vertices.empty ());
    EXPECT_TRUE (r.poche.vertices.empty ());
}

TEST (SectionCaps, ATangentPlaneIsNudgedIntoTheKeptSide)
{
    SectionMesh box;
    AddBox (box, 0.0f, 0.0f, 0.0f, 2.0f, 3.0f, 4.0f);
    // Exactly on the top face, keeping the low side: the cap is the box's own section.
    SectionCapResult r = Cut (box, SectionAxis::Z, 4.0f);
    EXPECT_EQ (r.counts.closedLoops, 1u);
    EXPECT_NEAR (r.counts.areaM2, 6.0, 1.0e-3);
    // Exactly on the bottom face, keeping the high side.
    r = Cut (box, SectionAxis::Z, 0.0f, /*flipped*/ true);
    EXPECT_EQ (r.counts.closedLoops, 1u);
    EXPECT_NEAR (r.counts.areaM2, 6.0, 1.0e-3);
}

TEST (SectionCaps, LocalFrameRoundTripsToWorld)
{
    const float world[3] = { 3.0f, -7.0f, 11.0f };
    for (const SectionAxis axis : { SectionAxis::X, SectionAxis::Y, SectionAxis::Z }) {
        double local[3];
        ToSectionLocal (axis, world, local);
        EXPECT_DOUBLE_EQ (local[2], double (world[int (axis)])) << "the cut axis is local z";
        float m[16];
        SectionLocalToWorld (axis, m);
        const float v[4] = { float (local[0]), float (local[1]), float (local[2]), 1.0f };
        float back[4];
        geomsrv::archviz::TransformPoint (back, v, m);
        for (int k = 0; k < 3; ++k)
            EXPECT_FLOAT_EQ (back[k], world[k]) << SectionAxisName (axis) << " component " << k;
    }
}

TEST (SectionCaps, LineIsAWorldWidthBandInsideTheCut)
{
    SectionMesh box;
    AddBox (box, 0.0f, 0.0f, 0.0f, 2.0f, 3.0f, 4.0f);
    SectionCapRequest request;
    request.coordinate = 1.0f;
    request.shape.lineWidth = 0.1f;
    request.shape.hatch = true;
    request.meshes.push_back (std::make_shared<SectionMesh> (box));
    const SectionCapResult r = BuildSectionCaps (request);
    // Inset by 0.1 m with mitred corners: 1.8 x 2.8 of poche, the rest is line.
    EXPECT_NEAR (SectionCapMeshArea (r.poche), 1.8 * 2.8, 1.0e-3);
    EXPECT_NEAR (SectionCapMeshArea (r.line), 6.0 - 1.8 * 2.8, 1.0e-3);
    EXPECT_GT (SectionCapMeshArea (r.hatch), 0.0);
    EXPECT_LT (SectionCapMeshArea (r.hatch), SectionCapMeshArea (r.poche));
}
