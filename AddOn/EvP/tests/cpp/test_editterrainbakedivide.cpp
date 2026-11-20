// tapioca-dev #177: contours divided by length keep clear of each other and of the outline.
#include "Edit/EditTerrainBakeDivide.hpp"
#include <gtest/gtest.h>
#include <cmath>

namespace ed = geomsrv::edit;

namespace {
constexpr double kPi = 3.14159265358979323846;

std::vector<ed::Vec3> Ring (double radius, double z, double phase, int n = 360)
{
    std::vector<ed::Vec3> ring;
    for (int i = 0; i < n; ++i) {
        const double a = phase + 2.0 * kPi * i / n;
        ring.push_back ({ radius * std::cos (a), radius * std::sin (a), z });
    }
    return ring;
}

std::vector<ed::Vec3> Square (double half)
{
    return { { -half, -half, 0 }, { half, -half, 0 }, { half, half, 0 }, { -half, half, 0 } };
}

double Orient (ed::Vec3 a, ed::Vec3 b, ed::Vec3 c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool Crosses (ed::Vec3 a, ed::Vec3 b, ed::Vec3 c, ed::Vec3 d)
{
    return Orient (a, b, c) * Orient (a, b, d) < 0.0 && Orient (c, d, a) * Orient (c, d, b) < 0.0;
}

// Every chord of every line against every chord of the others, brute force.
size_t CrossingsBetween (const std::vector<std::vector<ed::Vec3>>& lines)
{
    size_t count = 0;
    for (size_t i = 0; i < lines.size (); ++i)
        for (size_t j = i + 1; j < lines.size (); ++j)
            for (size_t p = 0; p < lines[i].size (); ++p)
                for (size_t q = 0; q < lines[j].size (); ++q)
                    count += Crosses (lines[i][p], lines[i][(p + 1) % lines[i].size ()], lines[j][q],
                                      lines[j][(q + 1) % lines[j].size ()]);
    return count;
}
} // namespace

TEST (EditTerrainBakeDivide, CloseRingsAreDividedMoreFinelyOnlyWhereTheyWouldCross177)
{
    // Two levels 0.3 m apart on a 10 m mound, traced out of phase, and a far ring 40 m away.
    const std::vector<std::vector<ed::Vec3>> traced { Ring (10.0, 1.0, 0.0), Ring (10.3, 0.5, kPi / 8.0),
                                                      Ring (3.0, 0.5, 0.0) };
    std::vector<std::vector<ed::Vec3>> farRing { traced[2] };
    for (auto& p : farRing[0])
        p.x += 40.0;
    std::vector<std::vector<ed::Vec3>> all { traced[0], traced[1], farRing[0] };

    // Divided alone each ring would be an octagon whose chords cut 0.75 m into the circle: the
    // outer one's would cross the inner one's.
    const auto divided = ed::DivideContours (all, { 1, 1, 1 }, Square (100.0), 8.0, 0.05);
    ASSERT_TRUE (divided.ok) << divided.error;
    ASSERT_EQ (divided.lines.size (), 3u);
    EXPECT_EQ (CrossingsBetween (divided.lines), 0u);
    EXPECT_EQ (divided.shortened, 2u) << "the two close rings, not the far one";
    EXPECT_EQ (divided.lines[2].size (), 3u) << "the far ring keeps its even step (19 m round, 8 m pieces, min 3)";
    EXPECT_LT (divided.lines[0].size (), 120u) << "still far lighter than the 360 traced points";
    // Every point is on its traced ring: its height is the level's.
    for (const auto& p : divided.lines[1]) {
        EXPECT_DOUBLE_EQ (p.z, 0.5);
        EXPECT_LE (std::hypot (p.x, p.y), 10.3 + 1e-9);
    }
}

TEST (EditTerrainBakeDivide, LinesApartStayEvenAndKeepTheirEnds177)
{
    // Two open contours 5 m apart, each ending on the outline.
    std::vector<std::vector<ed::Vec3>> traced (2);
    for (int i = 0; i <= 100; ++i) {
        traced[0].push_back ({ -20.0 + 0.4 * i, -2.5, 1.0 });
        traced[1].push_back ({ -20.0 + 0.4 * i, 2.5, 2.0 });
    }
    const auto divided = ed::DivideContours (traced, { 0, 0 }, Square (20.0), 4.0, 0.05);
    ASSERT_TRUE (divided.ok) << divided.error;
    EXPECT_EQ (divided.shortened, 0u);
    for (size_t l = 0; l < 2; ++l) {
        ASSERT_EQ (divided.lines[l].size (), 11u) << "40 m in 4 m pieces";
        EXPECT_EQ (divided.lines[l].front ().x, traced[l].front ().x);
        EXPECT_EQ (divided.lines[l].back ().x, traced[l].back ().x);
        for (size_t i = 0; i + 1 < divided.lines[l].size (); ++i)
            EXPECT_NEAR (divided.lines[l][i + 1].x - divided.lines[l][i].x, 4.0, 1e-9);
    }
}

TEST (EditTerrainBakeDivide, TracedLinesThatCrossAreRefusedWithTheirPlace177)
{
    const std::vector<std::vector<ed::Vec3>> traced { { { -10, -10, 1 }, { 10, 10, 1 } },
                                                      { { -10, 9, 2 }, { 10, -11, 2 } } };
    const auto divided = ed::DivideContours (traced, { 0, 0 }, Square (20.0), 5.0, 0.05);
    EXPECT_FALSE (divided.ok);
    EXPECT_NE (divided.error.find ("near ("), std::string::npos) << divided.error;
    EXPECT_FALSE (ed::DivideContours (traced, { 0, 0 }, Square (20.0), 0.0, 0.05).ok);
}
