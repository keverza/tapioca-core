#include "ArchViz/SectionCaps.hpp"
#include "ArchViz/SunStudyCylinderContour.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <map>

namespace {
using namespace geomsrv::archviz;
SectionMesh Mesh (std::vector<float> vertices, std::vector<uint32_t> indices)
{
    SectionMesh m;
    m.positions = std::move (vertices);
    m.indices = std::move (indices);
    for (size_t i = 0; i < m.positions.size (); ++i) {
        auto axis = i % 3;
        m.boundsMin[axis] = i < 3 ? m.positions[i] : std::min (m.boundsMin[axis], m.positions[i]);
        m.boundsMax[axis] = i < 3 ? m.positions[i] : std::max (m.boundsMax[axis], m.positions[i]);
    }
    return m;
}

TEST (SunStudyCylinderContour, VerticalWallAndTangentWallKeepNonzeroClippedSpan)
{
    for (float y : { 0.f, 1.f }) {
        CylinderContour contour;
        contour.Append (Mesh ({ -3, y, -4, 3, y, -4, 0, y, 5 }, { 0, 1, 2 }), { 0, 0, 1, -1, 1 });
        ASSERT_EQ (contour.segments.size (), y == 0 ? 2u : 1u);
        for (const auto& s : contour.segments) {
            EXPECT_EQ (s[0][0], s[1][0]);
            EXPECT_NEAR (std::hypot (s[0][0], y), 1, 1e-6);
            EXPECT_EQ (s[0][2], -1);
            EXPECT_EQ (s[1][2], 1);
        }
    }
}

TEST (SunStudyCylinderContour, AdjacentSlabFacesMakeClosedCircleAndDuplicateFacesAddNothing)
{
    auto mesh = Mesh ({ -2, -2, 0, 2, -2, 0, 2, 2, 0, -2, 2, 0 }, { 0, 1, 2, 0, 2, 3 });
    CylinderContour contour;
    contour.Append (mesh, { 0, 0, 1, -1, 1 });
    ASSERT_GT (contour.segments.size (), 50u);
    std::map<CylinderPoint, int> degree;
    for (const auto& s : contour.segments)
        for (const auto& p : s) {
            ++degree[p];
            EXPECT_NEAR (std::hypot (p[0], p[1]), 1, 1e-6);
        }
    for (const auto& p : degree)
        EXPECT_EQ (p.second, 2);
    auto count = contour.segments.size ();
    contour.Append (mesh, { 0, 0, 1, -1, 1 });
    EXPECT_EQ (contour.segments.size (), count);
}

TEST (SunStudyCylinderContour, SlopedFaceClipsAtBothHeightLimits)
{
    CylinderContour contour;
    contour.Append (Mesh ({ -4, -4, -4, 4, -4, 4, 0, 4, 0 }, { 0, 1, 2 }), { 0, 0, 1, -.4, .7 });
    ASSERT_FALSE (contour.segments.empty ());
    float lo = 1, hi = -1;
    for (const auto& s : contour.segments)
        for (const auto& p : s) {
            EXPECT_NEAR (p[0], p[2], 1e-6);
            lo = std::min (lo, p[2]);
            hi = std::max (hi, p[2]);
        }
    EXPECT_NEAR (lo, -.4, 1e-6);
    EXPECT_NEAR (hi, .7, 1e-6);
}

TEST (SunStudyCylinderContour, NoPointContactsOrInteriorOrExteriorLines)
{
    CylinderContour contour;
    for (float offset : { 0.f, 1.f, 4.f })
        contour.Append (Mesh ({ offset, 0, 0, offset + .1f, 0, 0, offset, .1f, 0 }, { 0, 1, 2 }), { 0, 0, 1, -1, 1 });
    contour.Append (Mesh ({ -2, -2, 0, 2, -2, 0, 0, 2, 0 }, { 0, 1, 2 }), { 0, 0, 0, -1, 1 });
    EXPECT_TRUE (contour.segments.empty ());
}
} // namespace
