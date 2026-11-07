#include "SunStudy/SunStudySolarCut.hpp"

#include "MeshFixtures.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace evp::sunstudy;

namespace {

constexpr double kRadians = 3.14159265358979323846 / 180.0;

SunStep Sun (double azimuthDegrees, double altitudeDegrees, int hour)
{
    SunStep step;
    step.time = TimeOfDay { hour, 0 };
    const double a = azimuthDegrees * kRadians, h = altitudeDegrees * kRadians;
    step.direction[0] = std::cos (h) * std::cos (a);
    step.direction[1] = std::cos (h) * std::sin (a);
    step.direction[2] = std::sin (h);
    step.altitudeDegrees = altitudeDegrees;
    return step;
}

// A single open quad: a free-standing screen with no thickness.
geomsrv::Mesh Screen (const std::string& guid, double x, double height)
{
    geomsrv::Mesh mesh;
    mesh.guid = guid;
    mesh.vertices = { x, -1, 0, x, 1, 0, x, 1, height, x, -1, height };
    mesh.triangles = { 0, 1, 2, 0, 2, 3 };
    return mesh;
}

} // namespace

TEST (SunStudySolarCut, TowerLosesExactlyWhatLiesAboveTheCuttingPlane)
{
    // A 2 x 2 x 10 m tower 5-7 m along +x; two suns at 45 degrees, +-15 degrees
    // of azimuth. Their rays span the plane z = x / cos(15); the cut runs 5 cm
    // under it so the rays clear what remains, so the removed volume is
    // 2 * integral over x in [5,7] of (10 - (x / cos 15 - 0.05)).
    geomsrv::Snapshot snapshot;
    snapshot.meshes = { evptest::MakeBox ("tower", 5, -1, 0, 2, 2, 10), evptest::MakeBox ("ignored", 5, 3, 0, 2, 2, 10),
                        evptest::MakeBox ("ground", -10, -10, -1, 20, 20, 1) };
    SolarCutInput input;
    input.steps = { Sun (-15, 45, 10), Sun (15, 45, 11) };
    input.hoursPerStep = 1.0;
    input.blocks = { 1, 0, 1 };
    input.ownMesh = 2; // the ground the point stands on
    SolarCutResult result;
    ASSERT_TRUE (ComputeSolarCut (snapshot, input, result, [] (const double*) { return false; })) << result.error;
    EXPECT_DOUBLE_EQ (result.facingHours, 2.0);
    EXPECT_DOUBLE_EQ (result.litHours, 0.0);
    ASSERT_EQ (result.objects.size (), 1u); // the ignored tower and the ground are not cut
    EXPECT_EQ (result.objects[0].guid, "tower");
    EXPECT_TRUE (result.objects[0].closed);
    const double slope = 1.0 / std::cos (15.0 * kRadians);
    const double exact = 2.0 * (10.0 * 2.0 - 0.5 * slope * (7.0 * 7.0 - 5.0 * 5.0)) + 2.0 * 2.0 * 0.05;
    const auto volumeOf = [] (const std::vector<SolarCutTriangle>& list) {
        double v = 0.0;
        for (const auto& t : list)
            v += (t.v[0][0] * (t.v[1][1] * t.v[2][2] - t.v[1][2] * t.v[2][1]) -
                  t.v[0][1] * (t.v[1][0] * t.v[2][2] - t.v[1][2] * t.v[2][0]) +
                  t.v[0][2] * (t.v[1][0] * t.v[2][1] - t.v[1][1] * t.v[2][0])) /
                 6.0;
        return v;
    };
    EXPECT_NEAR (result.removedVolume, exact, 1.0e-6)
        << "surfaces " << volumeOf (result.removed) << " caps " << volumeOf (result.caps) << " cap triangles "
        << result.caps.size (); // exact, not sampled
    EXPECT_NEAR (result.objects[0].total, 40.0, 1.0e-9);
    EXPECT_FALSE (result.caps.empty ());
    EXPECT_EQ (result.envelope.size (), 1u); // two suns: one sector, one cutting triangle
    for (const auto& piece : result.removed)
        for (const auto& corner : piece.v)
            EXPECT_GE (corner[2], corner[0] * slope - 0.05 - 1.0e-6); // nothing below the lowered plane is taken
    // What remains: the tower's lower part with its cut face, for the shadows.
    ASSERT_EQ (result.kept.size (), 1u);
    EXPECT_EQ (result.kept[0].first, 0u);
    EXPECT_GT (result.kept[0].second.TriangleCount (), 0u);
    double remains = 0.0;
    const auto& mesh = result.kept[0].second;
    for (size_t t = 0; t < mesh.TriangleCount (); ++t) {
        const double* a = &mesh.vertices[3ull * mesh.triangles[3 * t]];
        const double* b = &mesh.vertices[3ull * mesh.triangles[3 * t + 1]];
        const double* c = &mesh.vertices[3ull * mesh.triangles[3 * t + 2]];
        remains += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) +
                    a[2] * (b[0] * c[1] - b[1] * c[0])) /
                   6.0;
    }
    EXPECT_NEAR (std::abs (remains), 40.0 - exact, 1.0e-6); // removed + kept = the tower
}

TEST (SunStudySolarCut, OpenMeshShowsItsSurfacesButNoInventedVolume)
{
    geomsrv::Snapshot snapshot;
    snapshot.meshes = { Screen ("screen", 4.0, 10.0) };
    SolarCutInput input;
    input.steps = { Sun (-15, 45, 10), Sun (15, 45, 11) };
    input.blocks = { 1 };
    SolarCutResult result;
    ASSERT_TRUE (ComputeSolarCut (snapshot, input, result));
    ASSERT_EQ (result.objects.size (), 1u);
    EXPECT_FALSE (result.objects[0].closed);
    EXPECT_DOUBLE_EQ (result.objects[0].removed, 0.0);
    EXPECT_FALSE (result.removed.empty ());
}

TEST (SunStudySolarCut, SunBehindTheSurfaceOrASingleSunCutsNothing)
{
    geomsrv::Snapshot snapshot;
    snapshot.meshes = { evptest::MakeBox ("tower", 5, -1, 0, 2, 2, 10) };
    SolarCutInput input;
    const double west[3] = { -1.0, 0.0, 0.0 }; // a facade facing -x: the sun from +x is behind it
    input.normal = west;
    input.steps = { Sun (-15, 45, 10), Sun (15, 45, 11) };
    input.blocks = { 1 };
    SolarCutResult result;
    ASSERT_TRUE (ComputeSolarCut (snapshot, input, result));
    EXPECT_DOUBLE_EQ (result.facingHours, 0.0);
    EXPECT_TRUE (result.objects.empty ());

    input.normal = nullptr;
    input.steps = { Sun (0, 45, 10) }; // one sun: a line, not a surface
    ASSERT_TRUE (ComputeSolarCut (snapshot, input, result, [] (const double*) { return true; }));
    EXPECT_TRUE (result.envelope.empty ());
    EXPECT_TRUE (result.objects.empty ());
    EXPECT_DOUBLE_EQ (result.litHours, result.facingHours);
}

TEST (SunStudySolarCut, SurfaceStageMatchesTheCutsSurfaceAndClipsNothing)
{
    // The first, cheap stage: the same facing suns, hours and sectors as the
    // full cut, the envelope as long as asked, and nothing removed.
    geomsrv::Snapshot snapshot;
    snapshot.meshes = { evptest::MakeBox ("tower", 5, -1, 0, 2, 2, 10) };
    SolarCutInput input;
    input.steps = { Sun (-15, 45, 10), Sun (0, 45, 11), Sun (15, 45, 12) };
    input.hoursPerStep = 1.0;
    input.blocks = { 1 };
    SolarCutResult surface, cut;
    ASSERT_TRUE (ComputeSolarCutSurface (input, surface, [] (const double*) { return false; }, 12.0));
    ASSERT_TRUE (ComputeSolarCut (snapshot, input, cut, [] (const double*) { return false; }));
    EXPECT_DOUBLE_EQ (surface.facingHours, cut.facingHours);
    EXPECT_DOUBLE_EQ (surface.litHours, cut.litHours);
    ASSERT_EQ (surface.envelope.size (), 2u);
    EXPECT_EQ (surface.envelope.size (), cut.envelope.size ());
    EXPECT_TRUE (surface.removed.empty ());
    EXPECT_TRUE (surface.objects.empty ());
    const auto& fan = surface.envelope[0];
    const double reach = std::sqrt ((fan.v[1][0] - fan.v[0][0]) * (fan.v[1][0] - fan.v[0][0]) +
                                    (fan.v[1][1] - fan.v[0][1]) * (fan.v[1][1] - fan.v[0][1]) +
                                    (fan.v[1][2] - fan.v[0][2]) * (fan.v[1][2] - fan.v[0][2]));
    EXPECT_NEAR (reach, 12.0, 1.0e-9);
}

TEST (SunStudySolarCut, RadiusLowersTheCutterToClearTheWholeDisc)
{
    // The tower of the first test, cut for a 1 m disc about the point: every
    // plane drops by the disc's reach along it, so z = slope * (x - r) - 0.05,
    // and the removed volume is 2 * integral over [5,7] of (10 - z).
    geomsrv::Snapshot snapshot;
    snapshot.meshes = { evptest::MakeBox ("tower", 5, -1, 0, 2, 2, 10) };
    SolarCutInput input;
    input.steps = { Sun (-15, 45, 10), Sun (15, 45, 11) };
    input.hoursPerStep = 1.0;
    input.blocks = { 1 };
    SolarCutResult point, disc;
    ASSERT_TRUE (ComputeSolarCut (snapshot, input, point));
    input.radius = 1.0;
    ASSERT_TRUE (ComputeSolarCut (snapshot, input, disc)) << disc.error;
    const double slope = 1.0 / std::cos (15.0 * kRadians);
    EXPECT_NEAR (disc.removedVolume, 2.0 * (20.1 - slope * (12.0 - 2.0)), 1.0e-6);
    EXPECT_GT (disc.removedVolume, point.removedVolume);
    // The drawn cutter moves with it: its apex dropped along the plane normal
    // by r * sin(tilt), about 0.5 m down here.
    ASSERT_FALSE (disc.envelope.empty ());
    EXPECT_LT (disc.envelope[0].v[0][2], -0.4);
}
