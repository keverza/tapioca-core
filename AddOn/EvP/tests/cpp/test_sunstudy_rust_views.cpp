// Differential tests: the Rust visibility and viewpoint policies against
// RunVisibilityStudy and RunViewpointStudy over the same snapshot and the same
// C++ query index. Values, counts, grids and atlases are compared bit for bit;
// refusals must carry the same message.

#include "Geometry/QueryEngine.hpp"
#include "SunStudy/SunStudyCoreSelection.hpp"
#include "SunStudy/SunStudyRustViews.hpp"
#include "SunStudy/ViewpointStudy.hpp"
#include "SunStudy/VisibilityStudy.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace evp::sunstudy;
namespace rs = evp::sunstudy::rust;

class Lcg {
  public:
    explicit Lcg (uint64_t seed) : state_ (seed)
    {
    }
    double Uniform (double lo, double hi)
    {
        state_ = state_ * 6364136223846793005ull + 1442695040888963407ull;
        return lo + (hi - lo) * (static_cast<double> (state_ >> 11) * (1.0 / 9007199254740992.0));
    }

  private:
    uint64_t state_;
};

template <typename T> bool SameBits (const std::vector<T>& a, const std::vector<T>& b)
{
    return a.size () == b.size () && (a.empty () || std::memcmp (a.data (), b.data (), a.size () * sizeof (T)) == 0);
}

bool SameBits (double a, double b)
{
    return std::memcmp (&a, &b, sizeof a) == 0;
}

void AppendQuad (geomsrv::Mesh& mesh, const double o[3], const double u[3], const double v[3])
{
    const uint32_t base = static_cast<uint32_t> (mesh.vertices.size () / 3);
    for (int corner = 0; corner < 4; ++corner) {
        const double a = corner == 1 || corner == 2 ? 1.0 : 0.0, b = corner >= 2 ? 1.0 : 0.0;
        for (int axis = 0; axis < 3; ++axis)
            mesh.vertices.push_back (o[axis] + a * u[axis] + b * v[axis]);
    }
    mesh.triangles.insert (mesh.triangles.end (), { base, base + 1, base + 2, base, base + 2, base + 3 });
    mesh.triMaterial.assign (mesh.triangles.size () / 3, 0);
}

geomsrv::Mesh Box (const std::string& guid, double x, double y, double w, double d, double h)
{
    geomsrv::Mesh mesh;
    mesh.guid = guid;
    const double top[3] = { x, y, h }, ex[3] = { w, 0, 0 }, ey[3] = { 0, d, 0 }, ez[3] = { 0, 0, h };
    const double south[3] = { x, y, 0 }, north[3] = { x + w, y + d, 0 }, wx[3] = { -w, 0, 0 }, dy[3] = { 0, -d, 0 };
    AppendQuad (mesh, top, ex, ey);
    AppendQuad (mesh, south, ex, ez);
    AppendQuad (mesh, north, wx, ez);
    AppendQuad (mesh, south, ez, ey);
    AppendQuad (mesh, north, ez, dy);
    return mesh;
}

geomsrv::Mesh Floor (const std::string& guid, double x0, double y0, double w, double d)
{
    geomsrv::Mesh mesh;
    mesh.guid = guid;
    const double o[3] = { x0, y0, 0 }, u[3] = { w, 0, 0 }, v[3] = { 0, d, 0 };
    AppendQuad (mesh, o, u, v);
    return mesh;
}

const char* kFloor = "{0B000000-0000-0000-0000-000000000001}";
const char* kTower = "{0B000000-0000-0000-0000-000000000002}";
const char* kShed = "{0B000000-0000-0000-0000-000000000003}";
const char* kWall = "{0B000000-0000-0000-0000-000000000004}";

std::shared_ptr<const geomsrv::Snapshot> Scene (Lcg& lcg, uint64_t id)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = id;
    snapshot->meshes = { Floor (kFloor, -10, -10, 30, 25),
                         Box (kTower, lcg.Uniform (0, 4), lcg.Uniform (-2, 2), 4, 4, lcg.Uniform (6, 12)),
                         Box (kShed, lcg.Uniform (10, 14), lcg.Uniform (4, 8), 3, 2, 3),
                         Box (kWall, lcg.Uniform (-8, -6), -6, 0.4, 12, lcg.Uniform (2, 5)) };
    return snapshot;
}

void ExpectSameVisibility (const VisibilityStudyResult& e, const rs::RustVisibilityResult& a, const std::string& what)
{
    ASSERT_EQ (a.valid, e.valid) << what << ": cpp '" << e.error << "' rust '" << a.error << "'";
    if (!e.valid) {
        EXPECT_EQ (a.error, e.error) << what;
        return;
    }
    EXPECT_TRUE (SameBits (a.values, e.values)) << what << " values";
    EXPECT_EQ (a.aimPointCount, e.aimPointCount) << what;
    EXPECT_EQ (a.rayCount, e.rayCount) << what;
    EXPECT_EQ (a.visibleSamples, e.visibleSamples) << what;
    EXPECT_TRUE (SameBits (a.meanVisibility, e.meanVisibility)) << what << " mean";
    EXPECT_TRUE (SameBits (a.analysedArea, e.analysedArea)) << what << " area";
    EXPECT_EQ (a.displayElements, e.displayElements) << what;
    EXPECT_TRUE (SameBits (a.grid.positions, e.grid.positions)) << what << " triangle cells";
    EXPECT_TRUE (SameBits (a.patchGrid.positions, e.patchGrid.positions)) << what << " patch cells";
    EXPECT_EQ (a.atlas.width, e.atlas.width) << what;
    EXPECT_EQ (a.atlas.height, e.atlas.height) << what;
    EXPECT_EQ (a.atlas.texels, e.atlas.texels) << what;
    EXPECT_EQ (a.atlas.valid, e.atlas.valid) << what;
    ASSERT_EQ (a.patchAtlas.allocations.size (), e.patchAtlas.AllocationCount ()) << what;
    EXPECT_EQ (a.patchAtlas.width, e.patchAtlas.Width ()) << what;
    for (const auto& [key, x] : e.patchAtlas.Allocations ()) {
        const auto found = a.patchAtlas.allocations.find (key);
        ASSERT_NE (found, a.patchAtlas.allocations.end ()) << what;
        const auto& y = found->second;
        EXPECT_TRUE (x.x == y.x && x.y == y.y && x.width == y.width && x.height == y.height &&
                     x.generation == y.generation)
            << what << " allocation";
    }
}

} // namespace

TEST (SunStudyRustViewsParity, VisibilityMatchesAcrossOriginsDomainsAndContext)
{
    Lcg lcg (0x715b1ull);
    size_t cases = 0, partial = 0;
    for (int scene = 0; scene < 6; ++scene) {
        const auto snapshot = Scene (lcg, 100 + scene);
        geomsrv::QueryIndexCache::Get ().Release ();
        for (const auto origin : { VisibilityOrigin::Surfaces, VisibilityOrigin::Point }) {
            for (const auto domain : { SamplingDomain::SurfacePatch, SamplingDomain::TriangleLegacy }) {
                for (int context = 0; context < 3; ++context) {
                    VisibilityStudyOptions options;
                    options.origin = origin;
                    options.domain = domain;
                    options.spacing = scene % 2 == 0 ? 1.5 : 0.9;
                    options.maxAimPoints = scene % 3 == 0 ? 4 : 32; // forces the farthest-point reduction
                    options.explicitContext = context != 0;
                    if (context == 2)
                        options.contextElements = { kWall };
                    options.point[0] = lcg.Uniform (-5, 15);
                    options.point[1] = lcg.Uniform (-8, 12);
                    options.point[2] = lcg.Uniform (1, 4);
                    options.direction[0] = lcg.Uniform (-1, 1);
                    options.direction[1] = lcg.Uniform (-1, 1);
                    options.coneDegrees = lcg.Uniform (40, 170);
                    options.maxParallel = 1;
                    // 0 exposes the 1e-6 floor, 2 m exposes rays shorter than their tmin.
                    options.tmin = scene % 3 == 0 ? 0.001 : scene % 3 == 1 ? 0.0 : 2.0;
                    const std::vector<std::string> from = { kFloor }, to = { kTower, kShed };
                    const auto e = RunVisibilityStudy (snapshot, from, to, options);
                    const auto a = rs::RunVisibilityStudy (snapshot, from, to, options);
                    const std::string what = "scene " + std::to_string (scene) + " origin " +
                                             std::to_string (int (origin)) + " domain " +
                                             std::to_string (int (domain)) + " context " + std::to_string (context);
                    ExpectSameVisibility (e, a, what);
                    if (::testing::Test::HasFatalFailure ())
                        return;
                    ++cases;
                    for (const double v : e.values)
                        partial += v > 0.0 && v < 1.0 ? 1 : 0;
                    EXPECT_TRUE (e.valid) << what << ": " << e.error;
                }
            }
        }
    }
    EXPECT_EQ (cases, 72u);
    EXPECT_GT (partial, 50u) << "the corpus must produce partial visibility, not only 0 and 1";
}

TEST (SunStudyRustViewsParity, VisibilityRefusalsMatch)
{
    Lcg lcg (0x715b2ull);
    const auto snapshot = Scene (lcg, 7);
    struct Case {
        std::vector<std::string> from, to;
        VisibilityStudyOptions options;
    };
    std::vector<Case> cases;
    const VisibilityStudyOptions base;
    cases.push_back ({ { kFloor }, { kTower }, base });
    cases.back ().options.spacing = -1;
    cases.push_back ({ { kFloor }, { kTower }, base });
    cases.back ().options.maxRays = 0;
    cases.push_back ({ { kFloor }, {}, base });
    cases.push_back ({ {}, { kTower }, base });
    cases.push_back ({ { kTower }, { kTower }, base });
    cases.push_back ({ { kFloor }, { "{0B000000-0000-0000-0000-0000000000FF}" }, base });
    cases.push_back ({ { kFloor }, { kTower }, base });
    cases.back ().options.explicitContext = true;
    cases.back ().options.contextElements = { kTower };
    cases.push_back ({ { kFloor }, { kTower }, base });
    cases.back ().options.origin = VisibilityOrigin::Point;
    cases.back ().options.coneDegrees = 180;
    cases.push_back ({ { kFloor }, { kTower }, base });
    cases.back ().options.maxRays = 10; // fewer rays than cells
    cases.push_back ({ { kFloor }, { kTower, kShed }, base });
    cases.back ().options.maxRays = 900; // budget below one aim point per TO element
    cases.back ().options.spacing = 1.0;
    for (size_t i = 0; i < cases.size (); ++i) {
        const auto e = RunVisibilityStudy (snapshot, cases[i].from, cases[i].to, cases[i].options);
        const auto a = rs::RunVisibilityStudy (snapshot, cases[i].from, cases[i].to, cases[i].options);
        EXPECT_FALSE (e.valid) << "case " << i;
        ExpectSameVisibility (e, a, "refusal " + std::to_string (i));
    }
    const std::function<bool ()> cancelled = [] { return true; };
    const auto e = RunVisibilityStudy (snapshot, { kFloor }, { kTower }, base, cancelled);
    const auto a = rs::RunVisibilityStudy (snapshot, { kFloor }, { kTower }, base, cancelled);
    ExpectSameVisibility (e, a, "cancelled");
}

TEST (SunStudyRustViewsParity, ViewpointMatchesEveryBit)
{
    Lcg lcg (0x715b3ull);
    for (int scene = 0; scene < 8; ++scene) {
        const auto snapshot = Scene (lcg, 200 + scene);
        for (const auto& context : { std::vector<std::string> {}, std::vector<std::string> { kTower, kWall },
                                     std::vector<std::string> { kShed } }) {
            ViewpointOptions options;
            options.point = { lcg.Uniform (-5, 15), lcg.Uniform (-8, 12), lcg.Uniform (0.5, 3) };
            options.radius = lcg.Uniform (5, 30);
            options.rays = scene % 2 == 0 ? 720 : 97;
            const auto e = RunViewpointStudy (snapshot, context, options);
            const auto a = rs::RunViewpointStudy (snapshot, context, options);
            ASSERT_EQ (a.valid, e.valid) << e.error << " / " << a.error;
            EXPECT_EQ (a.collisionCount, e.collisionCount);
            EXPECT_TRUE (SameBits (a.area, e.area));
            ASSERT_EQ (a.perimeter.size (), e.perimeter.size ());
            EXPECT_EQ (
                std::memcmp (a.perimeter.data (), e.perimeter.data (), e.perimeter.size () * sizeof (double) * 3), 0);
        }
    }
    ViewpointOptions bad;
    bad.rays = 10;
    for (const auto& [snapshot, context, options] :
         { std::tuple { Scene (lcg, 300), std::vector<std::string> {}, bad },
           std::tuple { Scene (lcg, 301), std::vector<std::string> { "{0B000000-0000-0000-0000-0000000000FF}" },
                        ViewpointOptions () } }) {
        const auto e = RunViewpointStudy (snapshot, context, options);
        const auto a = rs::RunViewpointStudy (snapshot, context, options);
        EXPECT_FALSE (e.valid);
        EXPECT_EQ (a.valid, e.valid);
        EXPECT_EQ (a.error, e.error);
    }
}

TEST (SunStudyRustViewsParity, TheSelectedViewpointMatchesTheCppStudyUnderEitherCore)
{
    Lcg lcg (0x715b4ull);
    const auto snapshot = Scene (lcg, 400);
    ViewpointOptions options;
    options.point = { 2.0, 8.0, 1.6 };
    const std::vector<std::string> context { kTower, kWall };
    const auto expected = RunViewpointStudy (snapshot, context, options);
    for (const auto kind : { SunStudyCoreKind::Cpp, SunStudyCoreKind::Rust }) {
        OverrideSunStudyCore (kind);
        const auto actual = RunSelectedViewpointStudy (snapshot, context, options);
        OverrideSunStudyCore (std::nullopt);
        ASSERT_TRUE (actual.valid) << actual.error;
        EXPECT_EQ (actual.collisionCount, expected.collisionCount);
        EXPECT_TRUE (SameBits (actual.area, expected.area));
        EXPECT_EQ (std::memcmp (actual.perimeter.data (), expected.perimeter.data (),
                                expected.perimeter.size () * sizeof (double) * 3),
                   0);
    }
}
