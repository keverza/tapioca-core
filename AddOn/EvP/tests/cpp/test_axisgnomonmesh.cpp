// ArchViz/AxisGnomonMesh — the direction reference, checked as geometry.
//
// ⚠️ WHY OFFLINE: the gnomon exists to settle "is the image mirrored", which is
// the one rendering fault that otherwise looks perfectly fine. An instrument
// built to answer that question must not be capable of being wrong about it —
// and it very nearly is: each arrow is the same local geometry emitted through
// a different axis frame, and a frame whose (u x v) is not d is a REFLECTION.
// That reverses every triangle's winding while leaving the arrow looking
// correct, so the gnomon would keep pointing the right way while shading and
// culling inside out. Exactly the (x,y,z)->(x,z,y) bug class this repo has
// shipped once.

#include "ArchViz/AxisGnomonMesh.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace geomsrv::archviz;

namespace {

struct Gnomon {
    std::vector<ArchVizVertex> vertices;
    std::vector<uint16_t> indices;

    explicit Gnomon (float length = 2.0f, float thickness = 0.03f)
    {
        axisgnomon::Build (vertices, indices, length, thickness);
    }
};

// The signed volume contribution of a triangle fan from the origin. Summed over
// a CLOSED mesh whose faces are wound counter-clockwise seen from outside, it
// is six times the enclosed volume and therefore POSITIVE. One reflected arrow
// makes it drop by that arrow's share, so this catches a bad frame even when
// every individual triangle still agrees with its own stored normal.
double SignedVolumeTimesSix (const std::vector<ArchVizVertex>& v,
                             const std::vector<uint16_t>& idx)
{
    double total = 0.0;
    for (size_t t = 0; t + 2 < idx.size (); t += 3) {
        const ArchVizVertex& a = v[idx[t + 0]];
        const ArchVizVertex& b = v[idx[t + 1]];
        const ArchVizVertex& c = v[idx[t + 2]];
        total += double (a.x) * (double (b.y) * c.z - double (b.z) * c.y) -
                 double (a.y) * (double (b.x) * c.z - double (b.z) * c.x) +
                 double (a.z) * (double (b.x) * c.y - double (b.y) * c.x);
    }
    return total;
}

}   // namespace

TEST (AxisGnomonMesh, EveryTriangleAgreesWithItsStoredNormal)
{
    const Gnomon g;
    ASSERT_FALSE (g.indices.empty ());
    for (size_t t = 0; t < g.indices.size (); t += 3) {
        const ArchVizVertex& v0 = g.vertices[g.indices[t + 0]];
        const ArchVizVertex& v1 = g.vertices[g.indices[t + 1]];
        const ArchVizVertex& v2 = g.vertices[g.indices[t + 2]];

        const float e1[3] = {v1.x - v0.x, v1.y - v0.y, v1.z - v0.z};
        const float e2[3] = {v2.x - v0.x, v2.y - v0.y, v2.z - v0.z};
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                            e1[2] * e2[0] - e1[0] * e2[2],
                            e1[0] * e2[1] - e1[1] * e2[0]};
        const float agreement = n[0] * v0.nx + n[1] * v0.ny + n[2] * v0.nz;
        EXPECT_GT (agreement, 0.0f) << "triangle at index " << t
                                    << " is wound against its own normal";
    }
}

// The whole-mesh check the per-triangle one cannot make: three closed arrows,
// all wound outward, enclose a positive volume.
TEST (AxisGnomonMesh, TheArrowsEncloseAPositiveVolume)
{
    const Gnomon g;
    EXPECT_GT (SignedVolumeTimesSix (g.vertices, g.indices), 0.0);
}

// ⚠️ THE COLOUR CONVENTION IS THE INSTRUMENT. X red, Y green, Z blue is
// universal, so it needs no legend -- but the field is ABGR (0xAABBGGRR), and
// reading it as RGBA swaps X and Z. That would make the gnomon lie about
// exactly the thing it exists to settle.
TEST (AxisGnomonMesh, XIsRedYIsGreenZIsBlueWhenDecodedAsAbgr)
{
    const uint32_t x = axisgnomon::kAxisColorAbgr[0];
    const uint32_t y = axisgnomon::kAxisColorAbgr[1];
    const uint32_t z = axisgnomon::kAxisColorAbgr[2];

    auto red = [] (uint32_t c) { return c & 0xffu; };
    auto green = [] (uint32_t c) { return (c >> 8) & 0xffu; };
    auto blue = [] (uint32_t c) { return (c >> 16) & 0xffu; };
    auto alpha = [] (uint32_t c) { return (c >> 24) & 0xffu; };

    EXPECT_GT (red (x), green (x));
    EXPECT_GT (red (x), blue (x));
    EXPECT_GT (green (y), red (y));
    EXPECT_GT (green (y), blue (y));
    EXPECT_GT (blue (z), red (z));
    EXPECT_GT (blue (z), green (z));
    for (uint32_t c : {x, y, z})
        EXPECT_EQ (alpha (c), 0xffu) << "the gnomon is opaque";
}

// Each arrow must actually reach along its own axis and nowhere else, or the
// colours would be describing the wrong directions.
TEST (AxisGnomonMesh, EachArrowExtendsAlongItsOwnAxisOnly)
{
    constexpr float kLength = 2.0f;
    constexpr float kThickness = 0.05f;
    const Gnomon g (kLength, kThickness);

    // Group the vertices by colour, which is how an arrow is identified.
    for (int axis = 0; axis < 3; ++axis) {
        const uint32_t color = axisgnomon::kAxisColorAbgr[axis];
        float maxAlong = 0.0f;
        float maxAcross = 0.0f;
        size_t seen = 0;
        for (const ArchVizVertex& v : g.vertices) {
            if (v.abgr != color)
                continue;
            ++seen;
            const float p[3] = {v.x, v.y, v.z};
            for (int k = 0; k < 3; ++k) {
                if (k == axis)
                    maxAlong = std::max (maxAlong, p[k]);
                else
                    maxAcross = std::max (maxAcross, std::abs (p[k]));
            }
        }
        ASSERT_GT (seen, 0u) << "no vertices for axis " << axis;
        EXPECT_NEAR (maxAlong, kLength, 1e-4f) << axisgnomon::AxisName (axis) << " must reach its tip";
        // The head is three times the shaft's half-width; nothing may be wider.
        EXPECT_LE (maxAcross, kThickness * 3.0f + 1e-4f)
            << axisgnomon::AxisName (axis) << " bulges off its own axis";
    }
}

// An arrow points only in the POSITIVE direction: a gnomon with a tail is
// symmetric, and a symmetric instrument cannot detect a mirror.
TEST (AxisGnomonMesh, NoArrowExtendsBackwards)
{
    const Gnomon g (2.0f, 0.05f);
    for (int axis = 0; axis < 3; ++axis) {
        const uint32_t color = axisgnomon::kAxisColorAbgr[axis];
        for (const ArchVizVertex& v : g.vertices) {
            if (v.abgr != color)
                continue;
            const float p[3] = {v.x, v.y, v.z};
            EXPECT_GE (p[axis], -1e-4f) << axisgnomon::AxisName (axis) << " has geometry behind the origin";
        }
    }
}

TEST (AxisGnomonMesh, BuildAppendsRatherThanClearing)
{
    std::vector<ArchVizVertex> vertices;
    std::vector<uint16_t> indices;
    axisgnomon::Build (vertices, indices);
    const size_t first = vertices.size ();
    ASSERT_GT (first, 0u);
    axisgnomon::Build (vertices, indices);
    EXPECT_EQ (vertices.size (), first * 2)
        << "Build must append so several meshes can share one buffer";
}

TEST (AxisGnomonMesh, IndicesStayInsideTheVertexArray)
{
    const Gnomon g;
    for (uint16_t index : g.indices)
        EXPECT_LT (size_t (index), g.vertices.size ());
}

TEST (AxisGnomonMesh, OutOfRangeAxisNamesAreRefusedRatherThanRead)
{
    EXPECT_STREQ (axisgnomon::AxisName (-1), "");
    EXPECT_STREQ (axisgnomon::AxisName (3), "");
    EXPECT_STREQ (axisgnomon::AxisName (0), "+X east (red)");
}
