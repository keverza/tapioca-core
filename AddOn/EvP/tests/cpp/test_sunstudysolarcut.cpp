#include "SunStudy/SunStudySolarCut.hpp"

#include "MeshFixtures.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace evp::sunstudy;

namespace {

SunStep Sun (double azimuthDegrees, double altitudeDegrees, int hour)
{
    constexpr double kRadians = 3.14159265358979323846 / 180.0;
    SunStep step;
    step.time = TimeOfDay { hour, 0 };
    const double a = azimuthDegrees * kRadians, h = altitudeDegrees * kRadians;
    step.direction[0] = std::cos (h) * std::cos (a);
    step.direction[1] = std::cos (h) * std::sin (a);
    step.direction[2] = std::sin (h);
    step.altitudeDegrees = altitudeDegrees;
    return step;
}

} // namespace

TEST (SunStudySolarCut, EnvelopeTanCoversOnlyTheSlicesAzimuths)
{
    const std::vector<SunStep> steps = { Sun (-10, 30, 10), Sun (10, 50, 11) };
    double low = 0.0, high = 0.0;
    ASSERT_TRUE (SolarEnvelopeTan (steps, -0.01, 0.01, low, high));
    // Linear in tan(altitude) between the steps: halfway is the tangents' mean.
    const double mean = 0.5 * (std::tan (30.0 * 3.14159265358979 / 180.0) + std::tan (50.0 * 3.14159265358979 / 180.0));
    EXPECT_NEAR (low, mean, 0.03);
    EXPECT_NEAR (high, mean, 0.03);
    EXPECT_FALSE (SolarEnvelopeTan (steps, 1.0, 1.2, low, high)); // behind the slice: no ray there
    ASSERT_TRUE (SolarEnvelopeTan (steps, -3.2, 3.2, low, high)); // a full turn sees both
    EXPECT_LT (low, high);
}

TEST (SunStudySolarCut, TowerSouthOfThePointLosesItsTopAboveTheEnvelope)
{
    // A 2 x 2 x 10 m tower 5-7 m along +x; the sun from +x at 45 degrees for
    // two hours. Its rays are at height d there, so the tower must come down
    // to about 5-7 m; the slot alone is a far smaller volume.
    geomsrv::Snapshot snapshot;
    snapshot.meshes = { evptest::MakeBox ("tower", 5, -1, 0, 2, 2, 10), evptest::MakeBox ("ignored", 5, 3, 0, 2, 2, 10),
                        evptest::MakeBox ("ground", -10, -10, -1, 20, 20, 1) };
    SolarCutInput input;
    input.steps = { Sun (-15, 45, 10), Sun (15, 45, 11) }; // the wedge spans the whole tower
    input.hoursPerStep = 1.0;
    input.blocks = { 1, 0, 1 };
    input.ownMesh = 2; // the ground the point stands on
    input.voxel = 0.25;
    SolarCutResult result;
    ASSERT_TRUE (ComputeSolarCut (snapshot, input, result, [] (const double*) { return false; })) << result.error;
    EXPECT_DOUBLE_EQ (result.facingHours, 2.0);
    EXPECT_DOUBLE_EQ (result.litHours, 0.0);
    ASSERT_EQ (result.objects.size (), 1u); // the ignored tower and the ground are not cut
    EXPECT_EQ (result.objects[0].guid, "tower");
    // Analytic envelope cut of the tower: above z = x for x in [5,7], 2 m wide.
    const double analytic = 2.0 * (10.0 * 2.0 - 0.5 * (7.0 * 7.0 - 5.0 * 5.0)); // 2 * (20 - 12) = 16 m3
    EXPECT_GT (result.envelopeVolume, analytic * 0.9);
    EXPECT_LT (result.envelopeVolume, analytic * 1.6); // conservative at the voxel size, not wildly so
    EXPECT_GT (result.rayVolume, 0.0);
    EXPECT_LT (result.rayVolume, result.envelopeVolume * 0.6);
    for (const auto& voxel : result.removed) {
        EXPECT_GT (voxel.centre[2] + voxel.size * 0.5f, 4.5f); // nothing under the lowest ray is cut
        EXPECT_EQ (voxel.mesh, 0u);
    }
}

TEST (SunStudySolarCut, SunBehindTheSurfaceCannotBeWonAndLitStepsCount)
{
    geomsrv::Snapshot snapshot;
    snapshot.meshes = { evptest::MakeBox ("tower", 5, -1, 0, 2, 2, 10) };
    SolarCutInput input;
    const double west[3] = { -1.0, 0.0, 0.0 }; // a facade facing -x: the sun from +x is behind it
    input.normal = west;
    input.steps = { Sun (0, 45, 10) };
    input.blocks = { 1 };
    SolarCutResult result;
    ASSERT_TRUE (ComputeSolarCut (snapshot, input, result));
    EXPECT_DOUBLE_EQ (result.facingHours, 0.0);
    EXPECT_TRUE (result.removed.empty ());

    input.normal = nullptr;
    ASSERT_TRUE (ComputeSolarCut (snapshot, input, result, [] (const double*) { return true; }));
    EXPECT_DOUBLE_EQ (result.litHours, result.facingHours);
}
