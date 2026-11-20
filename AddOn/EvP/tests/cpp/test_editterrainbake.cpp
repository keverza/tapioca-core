// tapioca-dev #177: the built terrain planned as an Archicad Mesh -- an outline with heights and
// level lines that are polylines, reduced to a tolerance, joined to the outline where they end.
#include "Edit/EditTerrainBake.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

namespace {
namespace ed = geomsrv::edit;

struct Grid {
    std::vector<double> points;
    std::vector<uint32_t> triangles;
};

// `n` x `n` one-metre cells from the origin, heights from `z`; `skip` leaves a cell out.
Grid MakeGrid (int n, const std::function<double (double, double)>& z, const std::function<bool (int, int)>& skip = {})
{
    Grid g;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            g.points.insert (g.points.end (), { double (i), double (j), z (i, j) });
    const auto at = [n] (int i, int j) { return uint32_t (j * (n + 1) + i); };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            if (skip && skip (i, j))
                continue;
            g.triangles.insert (g.triangles.end (), { at (i, j), at (i + 1, j), at (i + 1, j + 1) });
            g.triangles.insert (g.triangles.end (), { at (i, j), at (i + 1, j + 1), at (i, j + 1) });
        }
    return g;
}

// A 2.3 m pad over 6..14 m with a 2 m batter down to level ground.
double Pad (double x, double y)
{
    const double d = std::max ({ 6.0 - x, x - 14.0, 6.0 - y, y - 14.0, 0.0 });
    return std::clamp ((2.0 - d) / 2.0, 0.0, 1.0) * 2.3;
}

bool Near (ed::Vec3 a, ed::Vec3 b)
{
    return std::abs (a.x - b.x) < 1e-9 && std::abs (a.y - b.y) < 1e-9 && std::abs (a.z - b.z) < 1e-9;
}
bool Joins (const ed::BakeLine& line, ed::Vec3 a, ed::Vec3 b)
{
    return line.points.size () == 2 && ((Near (line.points[0], a) && Near (line.points[1], b)) ||
                                        (Near (line.points[0], b) && Near (line.points[1], a)));
}
} // namespace

TEST (EditTerrainBake, ContoursAreClosedPolylinesReducedToTheTolerance)
{
    const auto g = MakeGrid (20, Pad);
    ed::BakeOptions options;
    options.interval = 0.5;
    const auto plan = ed::PlanTerrainBake (g.points, g.triangles, options);
    ASSERT_TRUE (plan.ok) << plan.error;
    EXPECT_DOUBLE_EQ (plan.lo, 0.0);
    EXPECT_DOUBLE_EQ (plan.hi, 2.3);
    // The square site: 80 edge points traced, its four corners kept.
    EXPECT_EQ (plan.outlineTraced, 80u);
    ASSERT_EQ (plan.outline.size (), 4u);
    // 0.5, 1.0, 1.5 and 2.0 m, each one ring around the batter. No loose points.
    ASSERT_EQ (plan.lines.size (), 4u);
    for (const auto& line : plan.lines) {
        EXPECT_EQ (line.kind, ed::BakeLineKind::Contour);
        EXPECT_TRUE (line.closed);
        EXPECT_GE (line.points.size (), 4u);
        EXPECT_LE (line.points.size (), 8u) << "a square ring keeps its corners only";
        EXPECT_GT (line.traced, line.points.size ());
        for (const auto& p : line.points)
            EXPECT_NEAR (p.z, line.points[0].z, 1e-12);
    }
    EXPECT_LT (plan.points, plan.traced);
    // The pad's own height is between two levels: the plan says a contour Mesh loses it.
    EXPECT_TRUE (std::any_of (plan.notes.begin (), plan.notes.end (),
                              [] (const std::string& n) { return n.find ("flat area") != std::string::npos; }));

    // A zero tolerance keeps every point that is not on a straight run.
    options.tolerance = 0.0;
    const auto exact = ed::PlanTerrainBake (g.points, g.triangles, options);
    ASSERT_TRUE (exact.ok);
    EXPECT_GE (exact.points, plan.points);
    EXPECT_EQ (exact.outline.size (), 4u);
}

TEST (EditTerrainBake, AnOpenContourEndsOnAnOutlineVertex)
{
    // A plane rising 2 m over 20 m: 0.5, 1.0 and 1.5 m cross the site; 0 and 2 m only touch its
    // edge and are the outline's, not level lines.
    const auto g = MakeGrid (20, [] (double x, double) { return 0.1 * x; });
    ed::BakeOptions options;
    options.interval = 0.5;
    const auto plan = ed::PlanTerrainBake (g.points, g.triangles, options);
    ASSERT_TRUE (plan.ok) << plan.error;
    ASSERT_EQ (plan.lines.size (), 3u);
    EXPECT_EQ (plan.outline.size (), 10u) << "four corners and the six ends of the lines";
    for (const auto& line : plan.lines) {
        EXPECT_FALSE (line.closed);
        ASSERT_EQ (line.points.size (), 2u);
        for (const auto& end : line.points)
            EXPECT_TRUE (std::any_of (plan.outline.begin (), plan.outline.end (),
                                      [&] (const ed::Vec3& v) { return Near (v, end); }))
                << "a line ends at " << end.x << ", " << end.y << " where the outline has no vertex";
    }
    // Counter-clockwise.
    double twice = 0;
    for (size_t i = 0; i < plan.outline.size (); ++i) {
        const auto a = plan.outline[i], b = plan.outline[(i + 1) % plan.outline.size ()];
        twice += a.x * b.y - b.x * a.y;
    }
    EXPECT_NEAR (twice / 2, 400.0, 1e-9);
}

// The user, 2026-11-20: a light Mesh whose contours are even polylines, divided by length. Each
// contour is cut into equal pieces along its traced length, whatever detail the surface had; a
// ring keeps at least three, an open line keeps its ends on the boundary.
TEST (EditTerrainBake, DivideByLengthCutsEachContourIntoEqualPieces177)
{
    const auto length = [] (const std::vector<ed::Vec3>& points, bool closed) {
        double total = 0.0;
        for (size_t i = 0; i + (closed ? 0 : 1) < points.size (); ++i) {
            const auto a = points[i], b = points[(i + 1) % points.size ()];
            total += std::hypot (b.x - a.x, b.y - a.y);
        }
        return total;
    };
    // Rings: the pad's batter, a fine 0.25 m grid so each traced contour has many points.
    const auto g = MakeGrid (80, [] (double x, double y) { return Pad (x / 4, y / 4); });
    ed::BakeOptions options;
    options.interval = 0.5;
    const auto traced = ed::PlanTerrainBake (g.points, g.triangles, [] {
        ed::BakeOptions exact;
        exact.interval = 0.5;
        exact.tolerance = 0.0;
        return exact;
    }());
    ASSERT_TRUE (traced.ok) << traced.error;
    options.divide = 5.0;
    const auto divided = ed::PlanTerrainBake (g.points, g.triangles, options);
    ASSERT_TRUE (divided.ok) << divided.error;
    ASSERT_EQ (divided.lines.size (), traced.lines.size ());
    for (size_t i = 0; i < divided.lines.size (); ++i) {
        const auto& line = divided.lines[i];
        ASSERT_TRUE (line.closed);
        const double perimeter = length (traced.lines[i].points, true);
        EXPECT_EQ (line.points.size (), size_t (std::max (3.0, std::round (perimeter / 5.0))))
            << "level " << line.points[0].z << ", traced " << perimeter << " m";
        for (const auto& p : line.points)
            EXPECT_NEAR (p.z, line.points[0].z, 1e-12) << "every point stays on its level";
    }
    // A step longer than a ring still leaves a triangle.
    options.divide = 1000.0;
    const auto coarse = ed::PlanTerrainBake (g.points, g.triangles, options);
    ASSERT_TRUE (coarse.ok);
    for (const auto& line : coarse.lines)
        EXPECT_EQ (line.points.size (), 3u);

    // Open lines: a plane rising 2 m over 20 m. Each level crosses the site in 20 m: four pieces
    // of 5 m, the ends exactly on the outline's vertices.
    const auto plane = MakeGrid (20, [] (double x, double) { return 0.1 * x; });
    options.divide = 5.0;
    const auto open = ed::PlanTerrainBake (plane.points, plane.triangles, options);
    ASSERT_TRUE (open.ok) << open.error;
    ASSERT_EQ (open.lines.size (), 3u);
    for (const auto& line : open.lines) {
        EXPECT_FALSE (line.closed);
        ASSERT_EQ (line.points.size (), 5u);
        for (size_t k = 0; k < line.points.size (); ++k)
            EXPECT_NEAR (line.points[k].y,
                         line.points.front ().y + (line.points.back ().y - line.points.front ().y) * double (k) / 4.0,
                         1e-9);
        for (const auto& end : { line.points.front (), line.points.back () })
            EXPECT_TRUE (std::any_of (open.outline.begin (), open.outline.end (),
                                      [&] (const ed::Vec3& v) { return Near (v, end); }));
    }
}

// With no design given (or with `folds`), Signature still traces the surface's folds.
TEST (EditTerrainBake, WithoutADesignSignatureLinesAreTheSurfacesFolds)
{
    const auto g = MakeGrid (20, Pad);
    ed::BakeOptions options;
    options.mode = ed::BakeMode::Signature;
    const auto plan = ed::PlanTerrainBake (g.points, g.triangles, options);
    ASSERT_TRUE (plan.ok) << plan.error;
    EXPECT_TRUE (std::any_of (plan.notes.begin (), plan.notes.end (),
                              [] (const std::string& n) { return n.find ("No design lines") != std::string::npos; }));
    EXPECT_DOUBLE_EQ (plan.interval, 0.5) << "a round interval for about four levels of 2.3 m";
    size_t creases = 0, contours = 0;
    bool padEdge = false, toeEdge = false;
    for (const auto& line : plan.lines) {
        (line.kind == ed::BakeLineKind::Crease ? creases : contours) += 1;
        EXPECT_GE (line.points.size (), 2u);
        if (line.kind != ed::BakeLineKind::Crease)
            continue;
        // The pad's edge at its own height and the batter's toe, each one straight line.
        padEdge = padEdge || Joins (line, { 6, 6, 2.3 }, { 14, 6, 2.3 });
        // (The toe turns one cell early at two corners, where the grid's diagonals cut them.)
        for (size_t i = 1; i < line.points.size (); ++i) {
            const auto a = line.points[i - 1], b = line.points[i];
            toeEdge = toeEdge || (Near ({ a.x, a.y, a.z }, { a.x, 4, 0 }) && Near ({ b.x, b.y, b.z }, { b.x, 4, 0 }) &&
                                  std::abs (a.x - b.x) >= 10.0);
        }
    }
    EXPECT_TRUE (padEdge);
    EXPECT_TRUE (toeEdge);
    EXPECT_GE (creases, 12u) << "four pad edges, four toe edges, four hips";
    EXPECT_EQ (contours, 4u);
    // No fold anywhere under the threshold: a plane has no signature lines.
    const auto flat = MakeGrid (6, [] (double x, double y) { return 0.05 * x + 0.02 * y; });
    options.interval = 100.0;
    const auto none = ed::PlanTerrainBake (flat.points, flat.triangles, options);
    ASSERT_TRUE (none.ok);
    EXPECT_TRUE (none.lines.empty ());
    EXPECT_EQ (none.outline.size (), 4u);
}

namespace {
// Ground rising 0.05 m per metre east; a path paved level at 3 m over 10..30 x 12..18, in fill
// all along, with a 1:1 batter down to the ground.
double Ground (double x, double)
{
    return 0.05 * x;
}
double Road (double x, double y)
{
    const double d = std::max ({ 10.0 - x, x - 30.0, 12.0 - y, y - 18.0, 0.0 });
    return Ground (x, y) + std::max (3.0 - Ground (x, y) - d, 0.0);
}
struct Site {
    Grid grid;
    ed::BakeDesign design;
};
// 40 x 30 one-metre cells; the path's paved outline sampled every 0.5 m, as paving gives it.
Site RoadSite (double x0 = 10, double x1 = 30)
{
    Site site;
    site.grid = MakeGrid (
        40, [] (double x, double y) { return y > 30 ? 0.0 : Road (x, y); }, [] (int, int j) { return j >= 30; });
    for (size_t i = 0; i < site.grid.points.size (); i += 3)
        site.design.base.push_back (Ground (site.grid.points[i], site.grid.points[i + 1]));
    std::vector<ed::Vec3> ring;
    const auto run = [&] (ed::Vec3 a, ed::Vec3 b) {
        const int n = int (std::round (Length (b - a) / 0.5));
        for (int k = 0; k < n; ++k)
            ring.push_back (a + (b - a) * (double (k) / n));
    };
    run ({ x0, 12, 3 }, { x1, 12, 3 });
    run ({ x1, 12, 3 }, { x1, 18, 3 });
    run ({ x1, 18, 3 }, { x0, 18, 3 });
    run ({ x0, 18, 3 }, { x0, 12, 3 });
    site.design.outlines.push_back (ring);
    return site;
}
} // namespace

// #177 rework (the user, 2026-11-16): signature lines are what was modelled. A path needs two:
// its outline and its toe, where the batter meets the existing ground -- not the triangulation's
// short folds. The soft ground keeps a few contours, and they stay off the graded area.
TEST (EditTerrainBake, APathOnSlopedGroundIsItsOutlineAndItsToe177)
{
    const auto site = RoadSite ();
    ed::BakeOptions options;
    options.mode = ed::BakeMode::Signature;
    const auto plan = ed::PlanTerrainBake (site.grid.points, site.grid.triangles, options, site.design);
    ASSERT_TRUE (plan.ok) << plan.error;

    std::vector<const ed::BakeLine*> outlines, toes, contours;
    for (const auto& line : plan.lines) {
        EXPECT_NE (line.kind, ed::BakeLineKind::Crease) << "no fold is a line unless asked";
        (line.kind == ed::BakeLineKind::Outline ? outlines
         : line.kind == ed::BakeLineKind::Toe   ? toes
                                                : contours)
            .push_back (&line);
    }
    ASSERT_EQ (outlines.size (), 1u);
    ASSERT_EQ (toes.size (), 1u);
    const auto& outline = *outlines[0];
    EXPECT_TRUE (outline.closed);
    EXPECT_EQ (outline.points.size (), 4u) << "the paved rectangle keeps its corners only";
    for (const auto& p : outline.points)
        EXPECT_NEAR (p.z, 3.0, 1e-9) << "draped on the built surface: the path's own level";

    const auto& toe = *toes[0];
    EXPECT_TRUE (toe.closed) << "the batter meets the ground all round";
    EXPECT_LT (toe.points.size (), 40u) << "reduced to a polyline, not the triangulation";
    for (const auto& p : toe.points) {
        EXPECT_NEAR (p.z, Ground (p.x, p.y), 0.02) << "on the existing ground at " << p.x << ", " << p.y;
        EXPECT_FALSE (p.x > 10 && p.x < 30 && p.y > 12 && p.y < 18) << "outside the paved area";
        EXPECT_GT (std::max ({ 10.0 - p.x, p.x - 30.0, 12.0 - p.y, p.y - 18.0 }), 1.0)
            << "out where the batter ends, not on the path's edge";
    }

    EXPECT_FALSE (contours.empty ()) << "the soft ground keeps a few contours";
    for (const auto* line : contours)
        for (const auto& p : line->points)
            EXPECT_NEAR (p.z, Ground (p.x, p.y), 1e-9) << "a contour of the ground the design left alone";
    EXPECT_TRUE (ed::CheckBakePayload (ed::TerrainBakePayload (plan)).empty ());

    // Asked for, the folds come back beside the design's lines.
    options.folds = true;
    const auto folded = ed::PlanTerrainBake (site.grid.points, site.grid.triangles, options, site.design);
    ASSERT_TRUE (folded.ok);
    EXPECT_TRUE (std::any_of (folded.lines.begin (), folded.lines.end (),
                              [] (const ed::BakeLine& l) { return l.kind == ed::BakeLineKind::Crease; }));
}

// A path that runs off the built surface is cut where it leaves; a piece shorter than
// `shortest` is a fragment and is left out.
TEST (EditTerrainBake, ADesignOutlineLeavingTheSurfaceIsCutAndFragmentsDropped177)
{
    auto site = RoadSite ();
    // A second ring almost wholly east of the 40 m site: only a 0.5 m stub of it lies on the surface.
    site.design.outlines.push_back ({ { 39.5, 2, 0 }, { 60, 2, 0 }, { 60, 2.5, 0 }, { 39.5, 2.5, 0 } });
    ed::BakeOptions options;
    options.mode = ed::BakeMode::Signature;
    auto plan = ed::PlanTerrainBake (site.grid.points, site.grid.triangles, options, site.design);
    ASSERT_TRUE (plan.ok) << plan.error;
    size_t outlines = 0;
    for (const auto& line : plan.lines)
        if (line.kind == ed::BakeLineKind::Outline) {
            ++outlines;
            for (const auto& p : line.points)
                EXPECT_LE (p.x, 40.0 + 1e-9) << "nothing off the surface";
        }
    EXPECT_EQ (outlines, 1u) << "the stub is shorter than a metre";
    options.shortest = 0.0;
    plan = ed::PlanTerrainBake (site.grid.points, site.grid.triangles, options, site.design);
    ASSERT_TRUE (plan.ok);
    outlines = 0;
    for (const auto& line : plan.lines)
        outlines += line.kind == ed::BakeLineKind::Outline;
    EXPECT_GT (outlines, 1u) << "kept when no shortest length is asked";
}

// #177 (live 2026-11-16): a raised point of the existing Mesh, away from the design, was lost --
// contours ringed it and the Mesh got a flat top. The untouched ground keeps its own shape: a
// sharp point becomes the folds through it, a gentle one (under the fold angle) a line down its
// steepest edge; level ground gets no line at all.
TEST (EditTerrainBake, TheUntouchedGroundKeepsItsPeaksAsLines177)
{
    auto site = RoadSite ();
    const auto raise = [&] (int i, int j, double by) {
        const size_t v = size_t (j * 41 + i);
        site.grid.points[v * 3 + 2] += by;
        site.design.base[v] += by; // existing ground: the design did not move it
        return ed::Vec3 { double (i), double (j), site.grid.points[v * 3 + 2] };
    };
    const ed::Vec3 sharp = raise (35, 25, 1.2), gentle = raise (5, 25, 0.06);
    ed::BakeOptions options;
    options.mode = ed::BakeMode::Signature;
    const auto plan = ed::PlanTerrainBake (site.grid.points, site.grid.triangles, options, site.design);
    ASSERT_TRUE (plan.ok) << plan.error;
    for (const auto& peak : { sharp, gentle }) {
        size_t through = 0;
        for (const auto& line : plan.lines)
            if (line.kind == ed::BakeLineKind::Crease &&
                std::any_of (line.points.begin (), line.points.end (),
                             [&] (const ed::Vec3& p) { return Near (p, peak); }))
                ++through;
        EXPECT_GE (through, 1u) << "the peak at " << peak.x << ", " << peak.y << " is a point of a line";
    }
    for (const auto& line : plan.lines)
        if (line.kind == ed::BakeLineKind::Crease)
            for (const auto& p : line.points)
                EXPECT_LE (
                    std::min (std::hypot (p.x - sharp.x, p.y - sharp.y), std::hypot (p.x - gentle.x, p.y - gentle.y)),
                    1.5)
                    << "level ground keeps no line: " << p.x << ", " << p.y;
    EXPECT_TRUE (ed::CheckBakePayload (ed::TerrainBakePayload (plan)).empty ());
}

// The user, 2026-11-18: Signature's soft ground was hatched with fold lines and fans. Given the
// existing ground's own vertices, the untouched ones are level points: the ground exact, both peaks
// kept at their heights, and no line but the design's, its toe and the contours. Nothing under the
// design or on the boundary is a point; the payload sends them as one-point ridges.
TEST (EditTerrainBake, SignatureKeepsTheExistingGroundAsItsOwnPoints)
{
    auto site = RoadSite ();
    const auto raise = [&] (int i, int j, double by) {
        const size_t v = size_t (j * 41 + i);
        site.grid.points[v * 3 + 2] += by;
        site.design.base[v] += by;
        return ed::Vec3 { double (i), double (j), site.grid.points[v * 3 + 2] };
    };
    const ed::Vec3 sharp = raise (35, 25, 1.2), gentle = raise (5, 25, 0.06);
    site.design.existing.assign (site.grid.points.size () / 3, 1);
    ed::BakeOptions options;
    options.mode = ed::BakeMode::Signature;
    const auto plan = ed::PlanTerrainBake (site.grid.points, site.grid.triangles, options, site.design);
    ASSERT_TRUE (plan.ok) << plan.error;
    for (const auto& line : plan.lines)
        EXPECT_NE (line.kind, ed::BakeLineKind::Crease) << "no fold line";
    for (const auto& peak : { sharp, gentle })
        EXPECT_TRUE (std::any_of (plan.levelPoints.begin (), plan.levelPoints.end (),
                                  [&] (const ed::Vec3& p) { return Near (p, peak); }))
            << "the peak at " << peak.x << ", " << peak.y << " is kept";
    ASSERT_FALSE (plan.levelPoints.empty ());
    for (const auto& p : plan.levelPoints) {
        EXPECT_NEAR (p.z,
                     Ground (p.x, p.y) + (Near (p, sharp)    ? 1.2
                                          : Near (p, gentle) ? 0.06
                                                             : 0.0),
                     1e-9)
            << "existing ground at " << p.x << ", " << p.y;
        EXPECT_TRUE (p.x > 0 && p.x < 40 && p.y > 0 && p.y < 30) << "not on the boundary";
        EXPECT_GT (std::max ({ 10.0 - p.x, p.x - 30.0, 12.0 - p.y, p.y - 18.0 }), 1.0) << "not under the design";
    }
    const auto payload = ed::TerrainBakePayload (plan);
    EXPECT_EQ (payload.levelPoints, plan.levelPoints.size ());
    EXPECT_TRUE (ed::CheckBakePayload (payload).empty ());
}

TEST (EditTerrainBake, ThePayloadIsMeasuredFromABasePlaneUnderTheLowestPoint)
{
    const auto g = MakeGrid (20, [] (double x, double y) { return 94.0 + Pad (x, y); });
    ed::BakeOptions options;
    options.interval = 0.5;
    const auto plan = ed::PlanTerrainBake (g.points, g.triangles, options);
    ASSERT_TRUE (plan.ok) << plan.error;
    const auto payload = ed::TerrainBakePayload (plan, 1.0);
    EXPECT_DOUBLE_EQ (payload.baseLevel, 93.0);
    ASSERT_EQ (payload.polyZ.size (), plan.outline.size ());
    for (const double z : payload.polyZ)
        EXPECT_NEAR (z, 1.0, 1e-9);
    ASSERT_EQ (payload.ridgeCounts.size (), plan.lines.size ());
    size_t at = 0;
    for (size_t i = 0; i < plan.lines.size (); ++i) {
        const auto n = size_t (payload.ridgeCounts[i]);
        EXPECT_EQ (n, plan.lines[i].points.size () + 1) << "a ring repeats its first point";
        EXPECT_DOUBLE_EQ (payload.ridgeCoords[at * 3], payload.ridgeCoords[(at + n - 1) * 3]);
        EXPECT_NEAR (payload.ridgeCoords[at * 3 + 2], plan.lines[i].points[0].z - 93.0, 1e-9);
        at += n;
    }
    EXPECT_EQ (at * 3, payload.ridgeCoords.size ());
    EXPECT_TRUE (ed::CheckBakePayload (payload).empty ());

    auto broken = payload;
    broken.ridgeCounts[0] = 1;
    EXPECT_FALSE (ed::CheckBakePayload (broken).empty ());
}

TEST (EditTerrainBake, AnOpeningIsCountedAndSaidAndNoSurfaceIsRefused)
{
    const auto g =
        MakeGrid (10, [] (double x, double) { return 0.1 * x; }, [] (int i, int j) { return i == 4 && j == 4; });
    const auto plan = ed::PlanTerrainBake (g.points, g.triangles, {});
    ASSERT_TRUE (plan.ok) << plan.error;
    EXPECT_EQ (plan.holes, 1u);
    EXPECT_TRUE (std::any_of (plan.notes.begin (), plan.notes.end (),
                              [] (const std::string& n) { return n.find ("inner opening") != std::string::npos; }));
    const auto none = ed::PlanTerrainBake ({}, {}, {});
    EXPECT_FALSE (none.ok);
    EXPECT_NE (none.error.find ("BakeSurfaceEmpty"), std::string::npos);
    EXPECT_TRUE (ed::TerrainBakePayload (none).outline.empty ());
}

// Mesh detail's As built: the surface as it is -- its kept edges one level line along the surface,
// every other vertex a level point, no other line (the user, 2026-11-18: filler lines through the
// points drew as stray lines across the ground). The payload sends each point as a one-point ridge
// last, and a one-point line anywhere else is still refused.
TEST (EditTerrainBake, AsBuiltKeepsItsLinesAndGivesEveryOtherPointAsALevelPoint)
{
    const auto g = MakeGrid (8, [] (double x, double y) { return Pad (x * 2.5, y * 2.5); });
    ed::BakeOptions options;
    options.mode = ed::BakeMode::Surface;
    ed::BakeDesign design;
    design.edges = { { 3 * 9 + 2, 3 * 9 + 3 }, { 3 * 9 + 3, 3 * 9 + 4 } }; // a kept line along y = 3
    const auto plan = ed::PlanTerrainBake (g.points, g.triangles, options, design);
    ASSERT_TRUE (plan.ok) << plan.error;
    std::set<std::pair<double, double>> on;
    for (const auto& p : plan.outline)
        on.emplace (p.x, p.y);
    ASSERT_EQ (plan.lines.size (), 1u) << "only the kept line";
    const auto& kept = plan.lines.front ();
    EXPECT_EQ (kept.kind, ed::BakeLineKind::Outline);
    ASSERT_EQ (kept.points.size (), 3u);
    for (const auto& p : kept.points) {
        EXPECT_EQ (p.y, 3.0);
        on.emplace (p.x, p.y);
    }
    for (const auto& p : plan.levelPoints)
        EXPECT_TRUE (on.emplace (p.x, p.y).second) << "a level point on no line";
    EXPECT_EQ (on.size (), g.points.size () / 3) << "every point of the surface is in the Mesh";
    EXPECT_EQ (plan.points, 3 + plan.levelPoints.size ());
    auto payload = ed::TerrainBakePayload (plan);
    EXPECT_EQ (payload.levelPoints, plan.levelPoints.size ());
    ASSERT_EQ (payload.ridgeCounts.size (), 1 + plan.levelPoints.size ());
    EXPECT_EQ (payload.ridgeCounts.front (), 3);
    EXPECT_TRUE (
        std::all_of (payload.ridgeCounts.begin () + 1, payload.ridgeCounts.end (), [] (int32_t n) { return n == 1; }));
    EXPECT_TRUE (ed::CheckBakePayload (payload).empty ());
    payload.levelPoints = 0; // the same points, as lines: refused
    EXPECT_FALSE (ed::CheckBakePayload (payload).empty ());
}
