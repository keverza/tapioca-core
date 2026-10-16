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

TEST (EditPathNetwork, EveryCornerOfTheAreaIsAKerbCorner)
{
    // The tee: A's four end corners, B's two far ones -- each its own path's -- and the two
    // re-entrant corners where B's sides meet A's left edge.
    const auto paths = Cross (0.0, true);
    const auto paved = ed::Pave (paths, 0.5, false);
    EXPECT_TRUE (paved.triangles.empty ()); // the outline and its corners only
    ASSERT_EQ (paved.corners.size (), 8u);
    size_t junctions = 0, outside = 0;
    for (const auto& c : paved.corners) {
        if (c.key.other != ed::kNoId) {
            ++junctions;
            EXPECT_EQ (c.path, paths[0].id); // kept on the older path
            EXPECT_EQ (c.key.other, paths[1].id);
            EXPECT_EQ (c.key.side, ed::KerbSide::Left);
            EXPECT_FALSE (c.convex);
            EXPECT_NEAR (c.at.y, 1.0, 1e-3);
        }
        else
            EXPECT_NE (c.key.point, ed::kNoId);
        outside += c.convex;
    }
    EXPECT_EQ (junctions, 2u);
    EXPECT_EQ (outside, 6u);
}

TEST (EditPathNetwork, ACornerSetApartIsRoundedAlone)
{
    ed::Document doc;
    doc.paths = Cross (0.0, true);
    const auto sharp = ed::Pave (doc.paths);
    const ed::PavedCorner* junction = nullptr;
    for (const auto& c : sharp.corners)
        if (c.key.other != ed::kNoId)
            junction = &c;
    ASSERT_NE (junction, nullptr);
    std::string error;
    ASSERT_TRUE (ed::SetKerbRadius (doc, junction->path, junction->key, 1.0, error)) << error;
    const auto round = ed::Pave (doc.paths);
    // One 1 m fillet: (1 - pi / 4) m2 more, and the corner reports its radius.
    EXPECT_NEAR (std::abs (Area (round.rings[0])) - std::abs (Area (sharp.rings[0])), 1.0 - 3.14159265 / 4.0, 0.01);
    size_t rounded = 0;
    for (const auto& c : round.corners)
        rounded += c.key.radius > 0.0;
    EXPECT_EQ (rounded, 1u);
    // The path's own radius sets its own corners; the junction keeps what it was given.
    ASSERT_TRUE (ed::SetPathEdgeRadius (doc, doc.paths[0].id, 0.5, error)) << error;
    ASSERT_EQ (doc.paths[0].kerbs.size (), 1u);
    for (const auto& c : ed::Pave (doc.paths).corners)
        if (c.key.other != ed::kNoId)
            EXPECT_NEAR (c.key.radius, c.key.Same (junction->key) ? 1.0 : 0.5, 1e-12);
}

TEST (EditPathNetwork, AKerbGripSetsTheRadiusItShows)
{
    auto paths = Cross (0.0, true);
    paths[0].edgeRadius = 0.6;
    const auto paved = ed::Pave (paths, 0.5, false), sharp = ed::Pave (Cross (0.0, true), 0.5, false);
    for (const auto& c : paved.corners) {
        const ed::Vec3 grip = ed::KerbGripAt (c, 0.05);
        EXPECT_NEAR (ed::KerbRadiusFromGrip (c, grip, 0.05), c.key.radius, 1e-9);
        // On its arc's middle: an outside corner's inside the sharp area, a re-entrant one's
        // in the notch it fills.
        EXPECT_EQ (Inside (sharp.rings[0], grip.x, grip.y), c.convex) << grip.x << ", " << grip.y;
    }
}
