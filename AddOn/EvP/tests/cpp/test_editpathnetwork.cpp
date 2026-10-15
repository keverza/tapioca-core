#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPathNetwork.hpp"
#include <gtest/gtest.h>
#include <cmath>

namespace ed = geomsrv::edit;

namespace {
double Area (const std::vector<ed::Vec3>& ring)
{
    double a = 0.0;
    for (size_t i = 0; i < ring.size (); ++i) {
        const auto& p = ring[i];
        const auto& q = ring[(i + 1) % ring.size ()];
        a += p.x * q.y - q.x * p.y;
    }
    return 0.5 * a;
}

bool Inside (const std::vector<ed::Vec3>& ring, double x, double y)
{
    bool in = false;
    for (size_t i = 0, j = ring.size () - 1; i < ring.size (); j = i++)
        if ((ring[i].y > y) != (ring[j].y > y) &&
            x < (ring[j].x - ring[i].x) * (y - ring[i].y) / (ring[j].y - ring[i].y) + ring[i].x)
            in = !in;
    return in;
}

double SurfaceArea (const ed::PavedArea& a)
{
    double sum = 0.0;
    for (size_t t = 0; t + 2 < a.triangles.size (); t += 3)
        sum += Area ({ a.vertices[a.triangles[t]], a.vertices[a.triangles[t + 1]], a.vertices[a.triangles[t + 2]] });
    return sum;
}

std::vector<ed::Path> Cross (double kerb, bool tee = false)
{
    ed::Document doc;
    const ed::Id a = ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 0 } }, 2.0);
    const ed::Id b = ed::AddPath (doc, { { 10, tee ? 0.0 : -10.0, 0 }, { 10, 10, 0 } }, 2.0);
    std::string error;
    EXPECT_TRUE (ed::SetPathEdgeRadius (doc, a, kerb, error)) << error;
    EXPECT_TRUE (ed::SetPathEdgeRadius (doc, b, kerb, error)) << error;
    return doc.paths;
}
} // namespace

TEST (EditPathNetwork, CrossingPathsPaveAsOneArea)
{
    const auto paved = ed::Pave (Cross (0.0));
    ASSERT_EQ (paved.rings.size (), 1u); // one + shaped outline, not two overlapping strips
    EXPECT_NEAR (std::abs (Area (paved.rings[0])), 20 * 2 + 20 * 2 - 2 * 2, 1e-3);
    EXPECT_NEAR (SurfaceArea (paved), std::abs (Area (paved.rings[0])), 1e-2);
}

TEST (EditPathNetwork, APathRunningIntoAnotherMakesATee)
{
    const auto paved = ed::Pave (Cross (0.0, true));
    ASSERT_EQ (paved.rings.size (), 1u);
    EXPECT_NEAR (std::abs (Area (paved.rings[0])), 20 * 2 + 10 * 2 - 2 * 1, 1e-3);
    EXPECT_FALSE (Inside (paved.rings[0], 10, -1.5)); // nothing of the joining path past the other's far edge
}

TEST (EditPathNetwork, KerbRadiiRoundJunctionsAndCorners)
{
    const auto sharp = ed::Pave (Cross (0.0)), round = ed::Pave (Cross (1.0));
    ASSERT_EQ (round.rings.size (), 1u);
    // A re-entrant corner of the junction is filled by a 1 m round...
    EXPECT_FALSE (Inside (sharp.rings[0], 8.8, 1.2));
    EXPECT_TRUE (Inside (round.rings[0], 8.8, 1.2));
    // ...and an outside corner at a path's end is rounded off (by at most 0.9 of its half width).
    EXPECT_TRUE (Inside (sharp.rings[0], 0.03, 0.97));
    EXPECT_FALSE (Inside (round.rings[0], 0.03, 0.97));
    EXPECT_GT (round.rings[0].size (), sharp.rings[0].size ());
}

TEST (EditPathNetwork, ThePavedSurfaceCarriesThePathsHeights)
{
    ed::Document doc;
    ed::AddPath (doc, { { 0, 0, 100 }, { 20, 0, 102 } }, 2.0);
    const auto paved = ed::Pave (doc.paths);
    ASSERT_FALSE (paved.vertices.empty ());
    for (const auto& v : paved.vertices) {
        const double centre = 100.0 + 2.0 * v.x / 20.0;        // the grade along it
        EXPECT_NEAR (v.z, centre, 0.05) << v.x << ", " << v.y; // a 2 % crossfall over 1 m: 2 cm
    }
}
