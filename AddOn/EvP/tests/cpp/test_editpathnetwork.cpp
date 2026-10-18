#include "Edit/EditPathCrossing.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPathNetwork.hpp"
#include <gtest/gtest.h>
#include <algorithm>
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

// Whether a plan point is inside the paved area: the rings' even-odd rule, so an outer ring
// and the holes in it contain a point an even number of times.
bool InArea (const ed::PavedArea& a, double x, double y)
{
    bool in = false;
    for (const auto& ring : a.rings)
        if (Inside (ring, x, y))
            in = !in;
    return in;
}

// Every triangle lies inside the paved outline and the triangles together fill its area: a
// triangulation that bridges a concave junction leaves the outline or changes the sum.
void ExpectMeshFillsItsOutline (const ed::PavedArea& a)
{
    ASSERT_FALSE (a.triangles.empty ());
    double outlined = 0.0;
    for (const auto& ring : a.rings)
        outlined += std::abs (Area (ring));
    for (size_t t = 0; t + 2 < a.triangles.size (); t += 3) {
        const auto& p = a.vertices[a.triangles[t]];
        const auto& q = a.vertices[a.triangles[t + 1]];
        const auto& r = a.vertices[a.triangles[t + 2]];
        const double area = Area ({ p, q, r });
        if (std::abs (area) < 1.0e-6)
            continue; // a collapsed triangle has no middle to test
        EXPECT_TRUE (InArea (a, (p.x + q.x + r.x) / 3.0, (p.y + q.y + r.y) / 3.0))
            << "triangle middle " << (p.x + q.x + r.x) / 3.0 << ", " << (p.y + q.y + r.y) / 3.0
            << " is outside the paved outline";
    }
    EXPECT_NEAR (std::abs (SurfaceArea (a)), outlined, 0.05);
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

TEST (EditPathNetwork, ACrossingHasFourJunctionCorners)
{
    // A slanted path across one that bends: the four corners where their sides cross are
    // junction corners, re-entrant, kept on the older path.
    ed::Document doc;
    const ed::Id a = ed::AddPath (doc, { { -20, 0, 0 }, { 15, 0, 0 }, { 16, 20, 0 } }, 2.0);
    std::string error;
    ASSERT_TRUE (ed::SetPathRadius (doc, a, ed::FindPath (doc, a)->points[1].id, 4.0, error)) << error;
    ed::AddPath (doc, { { -4, 12, 0 }, { -1, -6, 0 } }, 2.0);
    ed::FindPath (doc, a)->profile.back ().z = 2.0;
    const ed::Document before = doc;
    ed::LinkCrossings (doc, before); // with the grade break they share there
    ASSERT_EQ (ed::FindPath (doc, a)->profile.size (), 3u);
    const auto paved = ed::Pave (doc.paths, 0.5, false);
    size_t junctions = 0;
    for (const auto& c : paved.corners)
        if (c.key.other != ed::kNoId) {
            ++junctions;
            EXPECT_FALSE (c.convex);
            EXPECT_EQ (c.path, a);
        }
    EXPECT_EQ (junctions, 4u);
}

TEST (EditPathNetwork, ASharpBendsEdgeCornersAreKerbCorners)
{
    // An L: its four end corners and, at the bend, the outside and the inside corner of its
    // edges -- both its bend point's.
    ed::Document doc;
    const ed::Id a = ed::AddPath (doc, { { -20, 0, 0 }, { 0, 0, 0 }, { 0, 20, 0 } }, 2.0);
    const ed::Id bend = ed::FindPath (doc, a)->points[1].id;
    const auto paved = ed::Pave (doc.paths, 0.5, false);
    size_t atBend = 0;
    for (const auto& c : paved.corners) {
        EXPECT_NE (c.key.point, ed::kNoId) << c.at.x << ", " << c.at.y;
        atBend += c.key.point == bend;
    }
    EXPECT_EQ (paved.corners.size (), 6u);
    EXPECT_EQ (atBend, 2u);
}

TEST (EditPathNetwork, AFilletIsItsArcAloneAndKeepsToItsLegs)
{
    // A (along X) crossed at 30 degrees by B, with C parallel to A 4 m above it. The sharp
    // corner between A's top edge and B is rounded by its own arc only -- not by filling the
    // whole gap a 2.5 m disk cannot reach between A and C -- and that arc keeps to its half
    // of the leg up B to the next corner (B meeting C), so the radius asked is clamped.
    ed::Document doc;
    const double c30 = std::cos (0.5235987755982988), s30 = std::sin (0.5235987755982988);
    const ed::Id a = ed::AddPath (doc, { { -10, 0, 0 }, { 40, 0, 0 } }, 2.0);
    const ed::Id b = ed::AddPath (doc, { { 15 - 20 * c30, -20 * s30, 0 }, { 15 + 20 * c30, 20 * s30, 0 } }, 2.0);
    ed::AddPath (doc, { { -10, 6, 0 }, { 40, 6, 0 } }, 2.0);
    const auto area = [] (const ed::PavedArea& p) {
        double sum = 0.0;
        for (const auto& r : p.rings)
            sum += Area (r);
        return sum;
    };
    const ed::PavedArea sharp = ed::Pave (doc.paths, 0.5, false);
    const ed::PavedCorner* acute = nullptr;
    for (const auto& c : sharp.corners)
        if (c.Involves (a) && c.Involves (b) && !c.convex && c.half < 0.3 && c.at.y > 0.0)
            acute = &c;
    ASSERT_NE (acute, nullptr);
    std::string error;
    ASSERT_TRUE (ed::SetKerbRadius (doc, acute->path, acute->key, 2.5, error)) << error;
    const ed::PavedArea round = ed::Pave (doc.paths, 0.5, false);
    const ed::PavedCorner* now = nullptr;
    for (const auto& c : round.corners)
        if (c.path == acute->path && c.key.Same (acute->key))
            now = &c;
    ASSERT_NE (now, nullptr);
    // The leg up B to C is 4 / sin 30 = 8 m: half of it, at this angle, takes about 1.05 m.
    EXPECT_LT (now->key.radius, 2.5);
    EXPECT_DOUBLE_EQ (now->requestedRadius, 2.5);
    EXPECT_TRUE (now->Limited ());
    EXPECT_NEAR (now->key.radius, 0.49 * 8.0 * std::tan (now->half), 0.05);
    // The paved area grows by that fillet and nothing else.
    const double r = now->key.radius, h = now->half;
    const double fillet = r * r / std::tan (h) - 0.5 * r * r * (3.14159265358979 - 2.0 * h);
    EXPECT_NEAR (area (round) - area (sharp), fillet, 0.02);
    // Every other corner is as it was.
    for (const auto& c : round.corners)
        if (!(c.path == acute->path && c.key.Same (acute->key)))
            EXPECT_DOUBLE_EQ (c.key.radius, 0.0);
}

TEST (EditPathNetwork, ARequestedKerbRadiusRecoversWhenIntersectingPathsMove)
{
    ed::Document doc;
    const double cos30 = std::cos (0.5235987755982988), sin30 = std::sin (0.5235987755982988);
    const ed::Id a = ed::AddPath (doc, { { -10, 0, 0 }, { 40, 0, 0 } }, 2.0);
    const ed::Id b =
        ed::AddPath (doc, { { 15 - 20 * cos30, -20 * sin30, 0 }, { 15 + 20 * cos30, 20 * sin30, 0 } }, 2.0);
    const ed::Id across = ed::AddPath (doc, { { -10, 6, 0 }, { 40, 6, 0 } }, 2.0);
    ed::PavedCorner corner;
    for (const auto& c : ed::Pave (doc.paths, 0.5, false).corners)
        if (c.Involves (a) && c.Involves (b) && !c.convex && c.half < 0.3 && c.at.y > 0)
            corner = c;
    ASSERT_NE (corner.path, ed::kNoId);
    std::string error;
    ASSERT_TRUE (ed::SetKerbRadius (doc, corner.path, corner.key, 1.5, error)) << error;
    for (const double shift : { 12.0, -12.0 }) {
        const auto points = ed::FindPath (doc, across)->points;
        for (const auto& point : points)
            ASSERT_TRUE (ed::MovePathPoint (doc, across, point.id, point.p + ed::Vec3 { 0, shift, 0 }, error)) << error;
        bool found = false;
        for (const auto& c : ed::Pave (doc.paths, 0.5, false).corners)
            if (c.path == corner.path && c.key.Same (corner.key)) {
                found = true;
                EXPECT_DOUBLE_EQ (c.requestedRadius, 1.5);
                EXPECT_DOUBLE_EQ (c.key.radius, std::min (1.5, c.maxRadius));
                EXPECT_EQ (c.Limited (), shift < 0);
            }
        ASSERT_TRUE (found);
        EXPECT_DOUBLE_EQ (ed::FindPath (doc, corner.path)->kerbs[0].radius, 1.5);
    }
}

TEST (EditPathNetwork, ACornerUsesTheWidthItSitsInNotTheNarrowestAlongThePath)
{
    // A 4 m path pinched to 0.2 m in its middle: its end corners sit in the wide ranges and
    // most of that width may be used. The pinch somewhere else must not clamp them (the cap
    // was the path's narrowest half width, so it did).
    ed::Document doc;
    const ed::Id p = ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 0 } }, 4.0);
    std::string error;
    ed::Id pinch = ed::kNoId, after = ed::kNoId;
    ASSERT_TRUE (ed::SplitPathSection (doc, p, 8.0, error, &pinch)) << error;
    ASSERT_TRUE (ed::SplitPathSection (doc, p, 12.0, error, &after)) << error;
    ASSERT_TRUE (ed::SetPathWidth (doc, p, pinch, 0.1, 0.1, error)) << error;
    ASSERT_TRUE (ed::SetPathWidth (doc, p, after, 2.0, 2.0, error)) << error;
    ASSERT_TRUE (ed::SetPathEdgeRadius (doc, p, 1.5, error)) << error;
    const auto paved = ed::Pave (doc.paths, 0.5, false);
    ASSERT_EQ (paved.corners.size (), 4u); // the two ends' corners; a width step is not a corner
    size_t wide = 0;
    for (const auto& c : paved.corners)
        if (c.at.x < 1.0 && c.at.y > 1.0) {
            ++wide;
            EXPECT_DOUBLE_EQ (c.requestedRadius, 1.5);
            EXPECT_GT (c.maxRadius, 1.5);         // the 2 m half width it sits in allows it
            EXPECT_DOUBLE_EQ (c.key.radius, 1.5); // so the request fits, uncut
            EXPECT_FALSE (c.Limited ());
        }
    EXPECT_EQ (wide, 1u);
}

TEST (EditPathNetwork, AWidthChangeBesideACornerBoundsItsRounding)
{
    // A range boundary that widens the path 0.2 m before its end moves the kerb line: the end
    // corner's fillet stays tangent to the edge it is on and so stops at the step. That step
    // is a real boundary, not a synthetic corner to skip, and must keep bounding the rounding.
    ed::Document doc;
    const ed::Id p = ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 0 } }, 2.0);
    std::string error;
    ed::Id wide = ed::kNoId;
    ASSERT_TRUE (ed::SplitPathSection (doc, p, 19.8, error, &wide)) << error;
    ASSERT_TRUE (ed::SetPathWidth (doc, p, wide, 2.0, 2.0, error)) << error;
    ASSERT_TRUE (ed::SetPathEdgeRadius (doc, p, 1.0, error)) << error;
    const auto paved = ed::Pave (doc.paths, 0.5, false);
    size_t atEnd = 0;
    for (const auto& c : paved.corners)
        if (c.at.x > 19.0) {
            ++atEnd;
            EXPECT_DOUBLE_EQ (c.requestedRadius, 1.0); // the intent is kept
            EXPECT_LT (c.maxRadius, 0.2);              // the 0.2 m of edge before the step
            EXPECT_DOUBLE_EQ (c.key.radius, c.maxRadius);
            EXPECT_TRUE (c.Limited ());
        }
    EXPECT_EQ (atEnd, 2u);
}

TEST (EditPathNetwork, TheMeshCoversTheOutlineAtCrossings)
{
    // A + crossing, a tee and a crossing with a short arm past it: each union is concave at
    // the junction, where a triangulation that bridges the notch would leave a triangle
    // outside the outline.
    for (const auto& paths : { Cross (0.0), Cross (0.0, true) }) {
        SCOPED_TRACE ("crossing and tee");
        ExpectMeshFillsItsOutline (ed::Pave (paths, 0.5, true));
    }
    ed::Document shortArm;
    ed::AddPath (shortArm, { { 0, 0, 0 }, { 20, 0, 0 } }, 2.0);
    ed::AddPath (shortArm, { { 10, -10, 0 }, { 10, 0.6, 0 } }, 2.0); // 0.6 m past the edge
    SCOPED_TRACE ("short arm");
    ExpectMeshFillsItsOutline (ed::Pave (shortArm.paths, 0.5, true));
}

TEST (EditPathNetwork, TheMeshCoversAFilletedJunction)
{
    // The acute A/B/C fixture with a junction corner rounded 2.5 m: the fillet is added to
    // the area and the surface still fills exactly the outline, every triangle inside it.
    ed::Document doc;
    const double c30 = std::cos (0.5235987755982988), s30 = std::sin (0.5235987755982988);
    const ed::Id a = ed::AddPath (doc, { { -10, 0, 0 }, { 40, 0, 0 } }, 2.0);
    ed::AddPath (doc, { { 15 - 20 * c30, -20 * s30, 0 }, { 15 + 20 * c30, 20 * s30, 0 } }, 2.0);
    ed::AddPath (doc, { { -10, 6, 0 }, { 40, 6, 0 } }, 2.0);
    ed::PavedCorner junction;
    for (const auto& c : ed::Pave (doc.paths, 0.5, false).corners)
        if (c.Involves (a) && c.key.other != ed::kNoId) {
            junction = c;
            break;
        }
    ASSERT_NE (junction.key.other, ed::kNoId);
    std::string error;
    ASSERT_TRUE (ed::SetKerbRadius (doc, junction.path, junction.key, 2.5, error)) << error;
    ExpectMeshFillsItsOutline (ed::Pave (doc.paths, 0.5, true));
}
