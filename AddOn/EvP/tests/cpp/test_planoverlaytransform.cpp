#include "PlanOverlay/PlanTransformMath.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace po = geomsrv::planoverlay;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Mapping {
    po::PlanPoint origin { 0.0, 0.0 };
    double metresPerLogicalPixel = 0.01;
    double rotation = 0.0;

    po::PlanPoint At (double x, double y) const
    {
        const double c = std::cos (rotation);
        const double s = std::sin (rotation);
        return { origin.x + metresPerLogicalPixel * (x * c + y * s),
                 origin.y + metresPerLogicalPixel * (x * s - y * c) };
    }
};

po::PlanTransform Observe (double physicalWidth, double physicalHeight, double dpiScale, const Mapping& mapping,
                           po::PlanPoint repeatedDrift = {})
{
    const po::LogicalSampleRect rect = po::MakeLogicalSampleRect (physicalWidth, physicalHeight, dpiScale);
    const po::PlanPoint topLeft = mapping.At (0.0, 0.0);
    po::PlanPoint topLeftAgain = topLeft;
    topLeftAgain.x += repeatedDrift.x;
    topLeftAgain.y += repeatedDrift.y;
    return po::ObservePlanTransform (rect, topLeft, mapping.At (double (rect.right), 0.0),
                                     mapping.At (0.0, double (rect.bottom)), topLeftAgain);
}

void ExpectProjects (const po::PlanTransform& transform, const po::PlanPoint& model, double x, double y)
{
    const po::PlanPoint projected = po::Project (transform, model);
    EXPECT_NEAR (projected.x, x, 1e-7);
    EXPECT_NEAR (projected.y, y, 1e-7);
}

} // namespace

TEST (PlanOverlayTransform, DpiExplicitSamplesCover100To200PercentAndFractionalLogicalDimensions)
{
    for (const double dpi : { 1.0, 1.25, 1.5, 1.75, 2.0 }) {
        Mapping mapping;
        mapping.origin = { 123.25, -456.75 };
        const po::LogicalSampleRect rect = po::MakeLogicalSampleRect (1001.0, 743.0, dpi);
        const po::PlanTransform transform = Observe (1001.0, 743.0, dpi, mapping);
        ASSERT_TRUE (transform.valid) << transform.why << " at " << dpi;
        ExpectProjects (transform, mapping.At (double (rect.right), 0.0), double (rect.right) * dpi, 0.0);
        ExpectProjects (transform, mapping.At (0.0, double (rect.bottom)), 0.0, double (rect.bottom) * dpi);
    }
}

TEST (PlanOverlayTransform, InvertsAFullRotatedAffine)
{
    Mapping mapping;
    mapping.origin = { -320000.125, 4800000.875 };
    mapping.metresPerLogicalPixel = 0.0037;
    mapping.rotation = 0.63;
    const po::PlanTransform transform = Observe (1920.0, 1080.0, 1.5, mapping);
    ASSERT_TRUE (transform.valid) << transform.why;
    EXPECT_GT (std::fabs (transform.xy), 1.0);
    EXPECT_GT (std::fabs (transform.yx), 1.0);
    ExpectProjects (transform, mapping.At (217.4, 391.8), 217.4 * 1.5, 391.8 * 1.5);
}

TEST (PlanOverlayTransform, RejectsDegenerateMirrorShearAndAnisotropy)
{
    const po::LogicalSampleRect rect = po::MakeLogicalSampleRect (1000.0, 800.0, 1.0);
    const po::PlanPoint origin { 0.0, 0.0 };
    EXPECT_FALSE (po::ObservePlanTransform (rect, origin, origin, origin, origin).valid);
    EXPECT_FALSE (po::ObservePlanTransform (rect, origin, { 10.0, 0.0 }, { 0.0, 8.0 }, origin).valid);
    EXPECT_FALSE (po::ObservePlanTransform (rect, origin, { 10.0, 0.0 }, { 4.0, -8.0 }, origin).valid);
    EXPECT_FALSE (po::ObservePlanTransform (rect, origin, { 10.0, 0.0 }, { 0.0, -16.0 }, origin).valid);
}

TEST (PlanOverlayTransform, TornObservationPreservesPaintedTransformAndSettledViewConverges)
{
    Mapping first;
    first.origin = { 20.0, 30.0 };
    po::PlanTransform accepted = Observe (1600.0, 900.0, 1.5, first);
    ASSERT_TRUE (accepted.valid);
    const po::PlanTransform painted = accepted;

    Mapping moved = first;
    moved.origin.x += 1.0;
    const po::PlanTransform torn = Observe (1600.0, 900.0, 1.5, moved, { 0.01, 0.0 });
    EXPECT_FALSE (torn.valid);
    EXPECT_FALSE (po::AdoptPlanTransform (torn, accepted));
    EXPECT_DOUBLE_EQ (accepted.offsetX, painted.offsetX);

    const po::PlanTransform settled = Observe (1600.0, 900.0, 1.5, moved);
    ASSERT_TRUE (po::AdoptPlanTransform (settled, accepted));
    ExpectProjects (accepted, moved.At (0.0, 0.0), 0.0, 0.0);
}

TEST (PlanOverlayTransform, RoundedActualPixelsCatchHalfPixelBoundaryAndSuppressStablePixels)
{
    po::PlanTransform before;
    before.valid = true;
    before.xx = before.yy = 1.0;
    before.offsetX = 100.49;
    po::PlanTransform after = before;
    after.offsetX = 100.51;
    const std::vector<po::PlanPoint> geometry { { 0.0, 0.0 } };
    EXPECT_TRUE (po::RoundedProjectedPixelsChanged (before, after, geometry));

    after.offsetX = 100.499;
    EXPECT_FALSE (po::RoundedProjectedPixelsChanged (before, after, geometry));
}

TEST (PlanOverlayTransform, GeoreferencedPointsRemainSensitiveToFinalCoefficientChanges)
{
    po::PlanTransform before;
    before.valid = true;
    before.xx = 0.001;
    before.yy = -0.001;
    before.offsetX = 400.0;
    before.offsetY = 300.0;
    po::PlanTransform after = before;
    after.xx += 0.0000002;
    const std::vector<po::PlanPoint> geometry { { 3200000.0, 4800000.0 } };
    EXPECT_TRUE (po::RoundedProjectedPixelsChanged (before, after, geometry));
}

TEST (PlanOverlayTransform, OriginShiftAndResizeProduceTheNewProjectedPixels)
{
    Mapping mapping;
    mapping.origin = { 10.0, 20.0 };
    const po::PlanTransform initial = Observe (800.0, 600.0, 1.25, mapping);
    Mapping shifted = mapping;
    shifted.origin = { 12.0, 17.0 };
    const po::PlanTransform resized = Observe (1001.0, 701.0, 1.25, shifted);
    ASSERT_TRUE (initial.valid && resized.valid);
    const std::vector<po::PlanPoint> geometry { mapping.At (100.0, 100.0) };
    EXPECT_TRUE (po::RoundedProjectedPixelsChanged (initial, resized, geometry));
    ExpectProjects (resized, shifted.At (0.0, 0.0), 0.0, 0.0);
}
