// tapioca-dev #177: the surface an Archicad Mesh made from a bake plan has -- the light terrain the
// editor shows in place of the built one, with the plan's divided contours as its breaklines.
#include "Edit/EditTerrainBakeSurface.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace {
namespace ed = geomsrv::edit;

// `n` x `n` one-metre cells: a 2.3 m pad over 6..14 m with a 2 m batter to level ground.
void Pad (int n, std::vector<double>& points, std::vector<uint32_t>& triangles)
{
    const auto z = [] (double x, double y) {
        const double d = std::max ({ 6.0 - x, x - 14.0, 6.0 - y, y - 14.0, 0.0 });
        return std::clamp ((2.0 - d) / 2.0, 0.0, 1.0) * 2.3;
    };
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            points.insert (points.end (), { double (i), double (j), z (i, j) });
    const auto at = [n] (int i, int j) { return uint32_t (j * (n + 1) + i); };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            triangles.insert (triangles.end (), { at (i, j), at (i + 1, j), at (i + 1, j + 1) });
            triangles.insert (triangles.end (), { at (i, j), at (i + 1, j + 1), at (i, j + 1) });
        }
}
} // namespace

TEST (EditTerrainBakeSurface, TheMeshSurfaceIsLightAndKeepsEveryDividedContour177)
{
    std::vector<double> points;
    std::vector<uint32_t> triangles;
    Pad (20, points, triangles);
    ed::BakeOptions options;
    options.interval = 0.5;
    options.divide = 2.0;
    const auto plan = ed::PlanTerrainBake (points, triangles, options);
    ASSERT_TRUE (plan.ok) << plan.error;
    ASSERT_FALSE (plan.lines.empty ());

    ed::BakeSurface surface;
    std::string error;
    ASSERT_TRUE (ed::BakePlanSurface (plan, surface, error)) << error;
    EXPECT_LT (surface.triangles.size (), triangles.size ()) << "lighter than the built surface";

    // It covers the site: the outline's plan area.
    double area = 0.0;
    for (size_t t = 0; t + 2 < surface.triangles.size (); t += 3) {
        const double* a = &surface.points[3 * surface.triangles[t]];
        const double* b = &surface.points[3 * surface.triangles[t + 1]];
        const double* c = &surface.points[3 * surface.triangles[t + 2]];
        area += ((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])) / 2.0;
    }
    EXPECT_NEAR (area, 400.0, 1e-6);

    // Every point of a level line is a vertex, and every piece of it an edge: the Mesh's level
    // lines are where the surface folds.
    std::map<std::tuple<double, double, double>, uint32_t> vertex;
    for (uint32_t v = 0; v * 3 + 2 < surface.points.size (); ++v)
        vertex[{ surface.points[3 * v], surface.points[3 * v + 1], surface.points[3 * v + 2] }] = v;
    std::set<std::pair<uint32_t, uint32_t>> edges;
    for (size_t t = 0; t + 2 < surface.triangles.size (); t += 3)
        for (int k = 0; k < 3; ++k) {
            const auto a = surface.triangles[t + k], b = surface.triangles[t + (k + 1) % 3];
            edges.insert ({ std::min (a, b), std::max (a, b) });
        }
    for (const auto& line : plan.lines)
        for (size_t i = 0; i < line.points.size (); ++i) {
            const auto& p = line.points[i];
            const auto found = vertex.find ({ p.x, p.y, p.z });
            ASSERT_NE (found, vertex.end ()) << "a level-line point is not a vertex: " << p.x << ", " << p.y;
            if (!line.closed && i + 1 == line.points.size ())
                continue;
            const auto& q = line.points[(i + 1) % line.points.size ()];
            const auto next = vertex.find ({ q.x, q.y, q.z });
            ASSERT_NE (next, vertex.end ());
            EXPECT_TRUE (
                edges.count ({ std::min (found->second, next->second), std::max (found->second, next->second) }))
                << "a level-line piece is not an edge at " << p.x << ", " << p.y;
        }
}

TEST (EditTerrainBakeSurface, ALongDivideStillMakesASurface177)
{
    // The batter's levels lie about 0.4 m apart: pieces of 3 to 20 m would cut across each other
    // if each contour were divided alone.
    std::vector<double> points;
    std::vector<uint32_t> triangles;
    Pad (20, points, triangles);
    bool guarded = false;
    for (const double divide : { 3.0, 6.0, 10.0, 20.0 }) {
        ed::BakeOptions options;
        options.interval = 0.5;
        options.divide = divide;
        const auto plan = ed::PlanTerrainBake (points, triangles, options);
        ASSERT_TRUE (plan.ok) << divide << ": " << plan.error;
        ed::BakeSurface surface;
        std::string error;
        EXPECT_TRUE (ed::BakePlanSurface (plan, surface, error)) << divide << ": " << error;
        for (const auto& note : plan.notes)
            guarded = guarded || note.find ("divided more finely") != std::string::npos;
    }
    EXPECT_TRUE (guarded) << "some length had pieces that would have crossed, and says so";
}

TEST (EditTerrainBakeSurface, NoPlanNoSurface177)
{
    ed::BakePlan plan;
    ed::BakeSurface surface { { 1, 2, 3 }, { 0, 0, 0 } };
    std::string error;
    EXPECT_FALSE (ed::BakePlanSurface (plan, surface, error));
    EXPECT_FALSE (error.empty ());
    EXPECT_EQ (surface.points.size (), 3u) << "unchanged on refusal";
}
