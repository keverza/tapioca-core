// ArchViz/HudClip: a HUD triangle cut to the rectangle ImGui drew it under, as the scissor the
// overlays do not have would cut it -- a scrolled page's rows past its edges are not drawn
// (the user, 2026-10-03: the panel's page scrolls).

#include "ArchViz/HudClip.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace hc = geomsrv::archviz::hudclip;

namespace {

hc::Corner At (float x, float y, float u = 0.0f, float v = 0.0f, uint32_t col = 0xFFFFFFFFu)
{
    hc::Corner corner;
    corner.x = x;
    corner.y = y;
    corner.u = u;
    corner.v = v;
    corner.col = col;
    return corner;
}

double Area (const std::vector<hc::Corner>& triangles)
{
    double area = 0.0;
    for (size_t k = 0; k + 2 < triangles.size (); k += 3) {
        const hc::Corner &a = triangles[k], &b = triangles[k + 1], &c = triangles[k + 2];
        area += 0.5 * double ((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
    }
    return area;
}

} // namespace

TEST (HudClip, AWholeTriangleInsideIsKeptBitForBit)
{
    std::vector<hc::Corner> out;
    const hc::Corner a = At (1.0f, 1.0f, 0.1f, 0.2f, 0x11223344u), b = At (9.0f, 1.0f), c = At (1.0f, 9.0f);
    EXPECT_EQ (hc::Clip (a, b, c, { 0.0f, 0.0f, 10.0f, 10.0f }, out), 1u);
    ASSERT_EQ (out.size (), 3u);
    EXPECT_EQ (out[0].x, a.x);
    EXPECT_EQ (out[0].u, a.u);
    EXPECT_EQ (out[0].col, a.col);
}

TEST (HudClip, OneWhollyOutsideOrUnderAnEmptyRectangleIsDropped)
{
    std::vector<hc::Corner> out;
    EXPECT_EQ (hc::Clip (At (20.0f, 1.0f), At (30.0f, 1.0f), At (20.0f, 9.0f), { 0.0f, 0.0f, 10.0f, 10.0f }, out), 0u);
    EXPECT_EQ (hc::Clip (At (1.0f, 1.0f), At (9.0f, 1.0f), At (1.0f, 9.0f), { 5.0f, 5.0f, 5.0f, 9.0f }, out), 0u);
    EXPECT_TRUE (out.empty ());
}

// ⚠️ A ROW SCROLLED HALF OUT OF THE PAGE: what is left of a glyph quad is the part inside, its
// uv and colour exactly where they were across it.
TEST (HudClip, AGlyphQuadHalfPastTheEdgeKeepsItsInsideWithItsUvAndColour)
{
    // A quad 0..10 by 0..10, uv 0..1, cut at y = 4 (the page's top edge).
    const hc::Rect page { 0.0f, 4.0f, 100.0f, 100.0f };
    std::vector<hc::Corner> out;
    const hc::Corner tl = At (0.0f, 0.0f, 0.0f, 0.0f, 0xFF000000u), tr = At (10.0f, 0.0f, 1.0f, 0.0f, 0xFF000000u);
    const hc::Corner br = At (10.0f, 10.0f, 1.0f, 1.0f, 0xFF0000C8u), bl = At (0.0f, 10.0f, 0.0f, 1.0f, 0xFF0000C8u);
    hc::Clip (tl, tr, br, page, out);
    hc::Clip (tl, br, bl, page, out);
    ASSERT_FALSE (out.empty ());
    ASSERT_EQ (out.size () % 3, 0u);
    EXPECT_NEAR (Area (out), 60.0, 1e-3) << "10 x 6 of the quad left";
    for (const hc::Corner& c : out) {
        EXPECT_GE (c.y, 4.0f - 1e-4f);
        EXPECT_NEAR (c.u, c.x / 10.0f, 1e-5) << "u is still x across the quad";
        EXPECT_NEAR (c.v, c.y / 10.0f, 1e-5);
        const float blue = float (c.col & 0xFFu);
        EXPECT_NEAR (blue, 200.0f * c.y / 10.0f, 1.0f) << "the colour carried byte by byte";
        EXPECT_EQ (c.col >> 24, 0xFFu);
    }
}

// Cut by every edge at once: a fan of whole triangles, wound as the triangle was.
TEST (HudClip, ATriangleCutByAllFourEdgesIsAFanWoundAsItWas)
{
    std::vector<hc::Corner> out;
    const hc::Rect box { 0.0f, 0.0f, 10.0f, 10.0f };
    const uint32_t made = hc::Clip (At (-20.0f, 5.0f), At (5.0f, -20.0f), At (30.0f, 30.0f), box, out);
    EXPECT_GE (made, 2u);
    ASSERT_EQ (out.size (), size_t (made) * 3u);
    EXPECT_NEAR (std::fabs (Area (out)), 100.0, 1e-2) << "the triangle covers the whole box";
    const double whole = 0.5 * ((5.0 + 20.0) * (30.0 - 5.0) - (30.0 + 20.0) * (-20.0 - 5.0));
    for (size_t k = 0; k + 2 < out.size (); k += 3) {
        const hc::Corner &a = out[k], &b = out[k + 1], &c = out[k + 2];
        const double signedArea = 0.5 * double ((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
        EXPECT_TRUE (signedArea == 0.0 || (signedArea > 0.0) == (whole > 0.0)) << "the winding kept";
    }
}
