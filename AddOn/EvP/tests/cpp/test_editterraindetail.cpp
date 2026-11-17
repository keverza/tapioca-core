// Mesh detail: the built terrain shown and baked with far fewer triangles -- the existing ground's
// own vertices, fixed heights and the authored lines exact, every other built vertex within the
// tolerance -- and baked As built through every one of its points.
#include "Edit/EditTerrainBake.hpp"
#include "Edit/EditTerrainDetail.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <set>

namespace {
namespace lg = tapioca::geometry;
namespace ed = geomsrv::edit;

// A published terrain over a 40 x 40 m, 0.5 m grid: a 1:2 mound in the middle (the design moved
// it), level ground at 10 m round it (the existing ground: `base` equal). The native capture is
// the four corners and the middle of one side.
struct Scene {
    lg::PublishedTerrain published;
    std::vector<double> native;
    Scene ()
    {
        const int n = 80;
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i) {
                const double x = 0.5 * i, y = 0.5 * j;
                const double r = std::hypot (x - 20.0, y - 20.0);
                const double z = 10.0 + std::max (0.0, 3.0 - 0.5 * r);
                published.points.insert (published.points.end (), { x, y, z });
                published.base.push_back (r < 6.0 ? 10.0 : z);
            }
        const auto at = [n] (int i, int j) { return uint32_t (j * (n + 1) + i); };
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                published.triangles.insert (published.triangles.end (),
                                            { at (i, j), at (i + 1, j), at (i + 1, j + 1) });
                published.triangles.insert (published.triangles.end (),
                                            { at (i, j), at (i + 1, j + 1), at (i, j + 1) });
            }
        // A design line across the mound (a path edge), y = 20, x 14..26.
        for (int i = 28; i < 52; ++i)
            published.mappings.push_back (
                { lg::TerrainMappingKind::DesignLineEdge, 0, { 0, at (i, 40), at (i + 1, 40), 0, 0, 0 } });
        // A fixed height.
        published.locked.push_back ({ at (44, 44), published.points[3 * at (44, 44) + 2], 1, 1, true });
        for (const auto& [x, y] : { std::pair { 0.0, 0.0 }, std::pair { 40.0, 0.0 }, std::pair { 40.0, 40.0 },
                                    std::pair { 0.0, 40.0 }, std::pair { 20.0, 0.0 } })
            native.insert (native.end (), { x, y, 10.0 });
        published.revision = 7;
    }
};

// The detailed surface's height at (x, y); NaN off it.
double HeightOn (const lg::DetailedTerrain& d, double x, double y)
{
    for (size_t t = 0; t < d.triangles.size (); t += 3) {
        const auto P = [&] (int k) { return &d.points[3 * size_t (d.triangles[t + k])]; };
        const double *a = P (0), *b = P (1), *c = P (2);
        const double det = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
        if (det == 0.0)
            continue;
        const double wa = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / det;
        const double wb = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / det;
        const double wc = 1.0 - wa - wb;
        if (std::min ({ wa, wb, wc }) >= -1e-9)
            return wa * a[2] + wb * b[2] + wc * c[2];
    }
    return NAN;
}
} // namespace

TEST (EditTerrainDetail, FewerTrianglesTheFeaturesExactTheRestWithinTheTolerance)
{
    const Scene scene;
    const auto& built = scene.published;
    const auto features = lg::PublishedDetailFeatures (built, scene.native);
    EXPECT_EQ (features.lines.size (), 2u * 24u);
    EXPECT_EQ (std::count (features.keep.begin (), features.keep.end (), 44u * 81u + 44u), 1) << "the fixed height";
    EXPECT_EQ (std::count (features.keep.begin (), features.keep.end (), 0u), 1) << "a native vertex left as it was";
    EXPECT_EQ (features.keep.size (), 6u) << "the five native vertices and the fixed height: no moved one";

    lg::DetailedTerrain detailed;
    std::string error;
    ASSERT_TRUE (lg::DetailPublishedTerrain (built, scene.native, lg::kMeshDetailStandard, detailed, error)) << error;
    EXPECT_EQ (detailed.revision, 7u);
    EXPECT_EQ (detailed.builtTriangles, built.triangles.size () / 3);
    EXPECT_LT (detailed.triangles.size () * 10, built.triangles.size ())
        << detailed.triangles.size () / 3 << " of " << built.triangles.size () / 3;
    // Kept points are there, at their own positions and heights.
    const std::set<uint32_t> sources (detailed.source.begin (), detailed.source.end ());
    for (const auto v : features.keep)
        EXPECT_TRUE (sources.contains (v)) << v;
    EXPECT_TRUE (sources.contains (40u * 81u + 28u) && sources.contains (40u * 81u + 52u)) << "the line's ends";
    for (size_t i = 0; i < detailed.source.size (); ++i)
        for (int k = 0; k < 3; ++k)
            EXPECT_EQ (detailed.points[3 * i + k], built.points[3 * size_t (detailed.source[i]) + k]);
    // Every built vertex within the tolerance of the detailed surface.
    double worst = 0.0;
    for (size_t v = 0; v < built.points.size () / 3; ++v) {
        const double z = HeightOn (detailed, built.points[3 * v], built.points[3 * v + 1]);
        ASSERT_FALSE (std::isnan (z)) << "the detailed surface covers the built one";
        worst = std::max (worst, std::abs (z - built.points[3 * v + 2]));
    }
    EXPECT_LE (worst, lg::kMeshDetailStandard + 1e-9);

    // Baked As built: every detailed point on the outline or a level line, nothing a Mesh refuses.
    ed::BakeOptions options;
    options.mode = ed::BakeMode::Surface;
    ed::BakeDesign design;
    design.edges = detailed.lines;
    const auto plan = ed::PlanTerrainBake (detailed.points, detailed.triangles, options, design);
    ASSERT_TRUE (plan.ok) << plan.error;
    std::set<std::pair<double, double>> on;
    for (const auto& p : plan.outline)
        on.emplace (p.x, p.y);
    for (const auto& line : plan.lines) {
        EXPECT_GE (line.points.size (), 2u) << "never a loose point";
        for (const auto& p : line.points)
            on.emplace (p.x, p.y);
    }
    EXPECT_EQ (on.size (), detailed.points.size () / 3);
    EXPECT_TRUE (ed::CheckBakePayload (ed::TerrainBakePayload (plan)).empty ());

    EXPECT_FALSE (lg::DetailPublishedTerrain (built, scene.native, 0.0, detailed, error));
}
