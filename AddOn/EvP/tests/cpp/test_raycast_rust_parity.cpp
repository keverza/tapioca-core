// Raycast plan E0: the Rust scene's occlusion against QueryEngine::Occluded
// and CpuTraversal, ray by ray, over seeded corpora that hit the edges of the
// contract -- tmin/tmax bounds (zero, negative, NaN, tiny, inverted), rays
// along faces and edges and through shared vertices, coincident and
// degenerate triangles, signed-zero directions, and survey coordinates.
// The exit criterion is zero mismatches.

#include "Geometry/GeometryRustRaycast.hpp"
#include "Geometry/QueryEngine.hpp"
#include "SunStudy/CpuTraversal.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace {

using tapioca::geometry::RustRaycastScene;

struct Lcg {
    uint64_t state;
    double Unit ()
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double> (state >> 11) / 9007199254740992.0;
    }
    double Uniform (double lo, double hi)
    {
        return lo + (hi - lo) * Unit ();
    }
};

void AppendQuad (geomsrv::Mesh& mesh, const double o[3], const double u[3], const double v[3], int cells)
{
    const uint32_t base = static_cast<uint32_t> (mesh.vertices.size () / 3);
    for (int j = 0; j <= cells; ++j)
        for (int i = 0; i <= cells; ++i)
            for (int axis = 0; axis < 3; ++axis)
                mesh.vertices.push_back (o[axis] + u[axis] * i / cells + v[axis] * j / cells);
    for (int j = 0; j < cells; ++j)
        for (int i = 0; i < cells; ++i) {
            const uint32_t a = base + j * (cells + 1) + i, b = a + 1, c = a + cells + 1, d = c + 1;
            mesh.triangles.insert (mesh.triangles.end (), { a, b, d, a, d, c });
        }
}

geomsrv::Mesh Box (const double at[3], double w, double d, double h, int cells, bool inward)
{
    geomsrv::Mesh mesh;
    const double x = at[0], y = at[1], z = at[2];
    const double top[3] = { x, y, z + h }, south[3] = { x, y, z }, north[3] = { x + w, y + d, z };
    const double ex[3] = { w, 0, 0 }, ey[3] = { 0, d, 0 }, ez[3] = { 0, 0, h }, wx[3] = { -w, 0, 0 },
                 dy[3] = { 0, -d, 0 };
    AppendQuad (mesh, top, inward ? ey : ex, inward ? ex : ey, cells);
    AppendQuad (mesh, south, ez, ex, cells);
    AppendQuad (mesh, north, ez, wx, cells);
    AppendQuad (mesh, south, ey, ez, cells);
    AppendQuad (mesh, north, dy, ez, cells);
    return mesh;
}

std::shared_ptr<geomsrv::Snapshot> City (Lcg& lcg, size_t boxes, const double offset[3], uint64_t id)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = id;
    for (size_t b = 0; b < boxes; ++b) {
        // Half the boxes sit on a 0.5 m lattice so faces, edges and vertices
        // line up with each other and with the axis-aligned rays below.
        const bool snapped = b % 2 == 0;
        double at[3] = { lcg.Uniform (-30, 30), lcg.Uniform (-30, 30), 0.0 };
        if (snapped) {
            at[0] = std::round (at[0] * 2) / 2;
            at[1] = std::round (at[1] * 2) / 2;
        }
        for (int axis = 0; axis < 3; ++axis)
            at[axis] += offset[axis];
        const double w = snapped ? 0.5 * (1 + (b % 9)) : lcg.Uniform (0.3, 6);
        const double d = snapped ? 0.5 * (1 + (b % 7)) : lcg.Uniform (0.3, 6);
        snapshot->meshes.push_back (Box (at, w, d, lcg.Uniform (0.5, 15), 1 + static_cast<int> (b % 3), b % 5 == 0));
    }
    // A coincident duplicate of the first box, and degenerate triangles.
    if (!snapshot->meshes.empty ())
        snapshot->meshes.push_back (snapshot->meshes.front ());
    geomsrv::Mesh slivers;
    slivers.vertices = { offset[0],     offset[1],     offset[2] + 5, offset[0] + 3, offset[1] + 3,
                         offset[2] + 5, offset[0] + 6, offset[1] + 6, offset[2] + 5 };
    slivers.triangles = { 0, 1, 2, 0, 0, 1, 2, 2, 2 };
    snapshot->meshes.push_back (slivers);
    return snapshot;
}

struct Tally {
    size_t rays = 0, occluded = 0, mismatches = 0;
    std::string first;
};

void Compare (const geomsrv::QueryEngine& engine, const RustRaycastScene& scene, const double org[3],
              const double dir[3], double tmin, double tmax, Tally& tally)
{
    const bool expected = engine.Occluded (org, dir, tmin, tmax);
    uint8_t actual = 7;
    std::string error;
    ASSERT_TRUE (scene.OccludeDirectional (org, 1, dir, tmin, tmax, &actual, 1, {}, error)) << error;
    ++tally.rays;
    tally.occluded += expected ? 1 : 0;
    if ((actual != 0) != expected) {
        if (tally.mismatches++ == 0) {
            char text[512];
            std::snprintf (text, sizeof text,
                           "org %.17g %.17g %.17g dir %.17g %.17g %.17g tmin %.17g tmax %.17g cpp %d", org[0], org[1],
                           org[2], dir[0], dir[1], dir[2], tmin, tmax, expected ? 1 : 0);
            tally.first = text;
        }
    }
}

double Bound (Lcg& lcg, size_t which, bool far)
{
    switch (which % 7) {
        case 0:
            return 0.0;
        case 1:
            return far ? lcg.Uniform (0.5, 40) : 0.001;
        case 2:
            return far ? 0.0 : 2.0;
        case 3:
            return -1.0;
        case 4:
            return std::numeric_limits<double>::quiet_NaN ();
        case 5:
            return far ? 1e-7 : 1e-9;
        default:
            return far ? lcg.Uniform (0, 3) : lcg.Uniform (0, 3); // sometimes inverted
    }
}

std::unique_ptr<RustRaycastScene> Build (const geomsrv::Snapshot& snapshot)
{
    std::string error;
    auto scene = RustRaycastScene::Build (snapshot, error);
    EXPECT_NE (scene, nullptr) << error;
    return scene;
}

} // namespace

TEST (RaycastRustParity, RandomRaysMatchQueryEngineWithZeroMismatches)
{
    const double origin[3] = { 0, 0, 0 }, survey[3] = { 2600000.0, 1200000.0, 450.0 };
    for (const auto& [boxes, offset, seed] : std::vector<std::tuple<size_t, const double*, uint64_t>> {
             { 3, origin, 1 }, { 80, origin, 2 }, { 400, origin, 3 }, { 150, survey, 4 } }) {
        Lcg lcg { seed * 0x9e3779b97f4a7c15ull };
        const auto snapshot = City (lcg, boxes, offset, seed);
        const geomsrv::QueryEngine engine (snapshot);
        const auto scene = Build (*snapshot);
        ASSERT_NE (scene, nullptr);
        Tally tally;
        for (size_t i = 0; i < 60000; ++i) {
            const double org[3] = { offset[0] + lcg.Uniform (-35, 35), offset[1] + lcg.Uniform (-35, 35),
                                    offset[2] + lcg.Uniform (-1, 16) };
            double dir[3] = { lcg.Uniform (-1, 1), lcg.Uniform (-1, 1), lcg.Uniform (-1, 1) };
            if (i % 11 == 0)
                dir[i % 3] = (i % 2 == 0) ? 0.0 : -0.0; // a zero component: infinite slab inverses
            if (i % 97 == 0)
                dir[0] = dir[1] = dir[2] = 0.0;
            Compare (engine, *scene, org, dir, Bound (lcg, i, false), Bound (lcg, i / 7, true), tally);
        }
        EXPECT_EQ (tally.mismatches, 0u) << boxes << " boxes: " << tally.first;
        EXPECT_GT (tally.occluded, 100u) << "the corpus must have both outcomes";
        EXPECT_LT (tally.occluded, tally.rays - tally.rays / 10);
    }
}

TEST (RaycastRustParity, RaysAlongFacesEdgesAndThroughVerticesMatch)
{
    Lcg lcg { 0x5eed5eedull };
    const double origin[3] = { 0, 0, 0 };
    const auto snapshot = City (lcg, 60, origin, 9);
    const geomsrv::QueryEngine engine (snapshot);
    const auto scene = Build (*snapshot);
    ASSERT_NE (scene, nullptr);
    Tally tally;
    const double axes[8][3] = { { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 },    { -1, 0, 0 },
                                { 0, 1, 0 }, { 0, -1, 0 }, { -0.0, 0, 1 }, { 1, 1, 0 } };
    for (const auto& mesh : snapshot->meshes)
        for (size_t v = 0; v + 2 < mesh.vertices.size (); v += 3) {
            const double* p = &mesh.vertices[v];
            for (const auto& dir : axes) {
                // From a vertex, from just outside it along the axis, and from a
                // lattice point offset into the box.
                const double back[3] = { p[0] - dir[0] * 0.75, p[1] - dir[1] * 0.75, p[2] - dir[2] * 0.75 };
                Compare (engine, *scene, p, dir, 0.0, 0.0, tally);
                Compare (engine, *scene, back, dir, 0.0, 0.0, tally);
                Compare (engine, *scene, back, dir, 0.001, 0.75, tally);
                Compare (engine, *scene, back, dir, 0.0, 0.75, tally);
            }
        }
    EXPECT_EQ (tally.mismatches, 0u) << tally.first;
    EXPECT_GT (tally.occluded, 0u);
}

TEST (RaycastRustParity, TheDirectionalBatchMatchesCpuTraversalUnderEveryParallelism)
{
    Lcg lcg { 0xba7c4ull };
    const double origin[3] = { 0, 0, 0 };
    const auto snapshot = City (lcg, 200, origin, 11);
    const auto engine = std::make_shared<geomsrv::QueryEngine> (snapshot);
    const evp::sunstudy::CpuTraversal traversal (engine);
    const auto scene = Build (*snapshot);
    ASSERT_NE (scene, nullptr);
    std::vector<double> origins;
    for (double y = -32; y < 32; y += 0.35)
        for (double x = -32; x < 32; x += 0.35)
            origins.insert (origins.end (), { x, y, 0.1 });
    const size_t count = origins.size () / 3;
    for (const auto& dir :
         std::vector<std::array<double, 3>> { { 0.3, -0.2, 0.7 }, { -0.9, 0.1, 0.05 }, { 0, 0, 1 } }) {
        std::vector<uint8_t> expected (count, 9);
        traversal.OccludeDirectional (origins.data (), count, dir.data (), 0.001, 0.0, expected.data (), 1);
        for (const size_t parallel : { size_t (1), size_t (0), size_t (4) }) {
            std::vector<uint8_t> actual (count, 9);
            std::string error;
            ASSERT_TRUE (scene->OccludeDirectional (origins.data (), count, dir.data (), 0.001, 0.0, actual.data (),
                                                    parallel, {}, error))
                << error;
            EXPECT_EQ (actual, expected) << "parallel " << parallel;
        }
    }
}

TEST (RaycastRustParity, CancellationRefusalsAndEmptyScenes)
{
    Lcg lcg { 0xcafeull };
    const double origin[3] = { 0, 0, 0 };
    const auto snapshot = City (lcg, 20, origin, 12);
    const auto scene = Build (*snapshot);
    ASSERT_NE (scene, nullptr);
    std::vector<double> origins (3 * 5000, 0.5);
    std::vector<uint8_t> out (5000, 9);
    const double up[3] = { 0, 0, 1 };
    std::string error;
    EXPECT_FALSE (
        scene->OccludeDirectional (origins.data (), 5000, up, 0, 0, out.data (), 2, [] { return true; }, error));
    EXPECT_TRUE (error.empty ()) << "cancelled is not refused";
    EXPECT_TRUE (scene->OccludeDirectional (
        origins.data (), 5000, up, 0, 0, out.data (), 2, [] { return false; }, error))
        << error;

    geomsrv::Snapshot broken;
    broken.meshes.push_back (Box (origin, 1, 1, 1, 1, false));
    broken.meshes[0].triangles.push_back (999);
    broken.meshes[0].triangles.push_back (0);
    broken.meshes[0].triangles.push_back (1);
    EXPECT_EQ (RustRaycastScene::Build (broken, error), nullptr);
    EXPECT_NE (error.find ("vertex that does not exist"), std::string::npos) << error;
    broken.meshes[0].vertices[2] = std::numeric_limits<double>::infinity ();
    EXPECT_EQ (RustRaycastScene::Build (broken, error), nullptr);

    const geomsrv::Snapshot empty;
    const auto none = Build (empty);
    ASSERT_NE (none, nullptr);
    uint8_t bit = 9;
    EXPECT_TRUE (none->OccludeDirectional (origins.data (), 1, up, 0, 0, &bit, 1, {}, error));
    EXPECT_EQ (bit, 0) << "an empty scene blocks nothing";
}
