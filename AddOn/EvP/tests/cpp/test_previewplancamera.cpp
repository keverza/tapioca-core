#include "Palette/PreviewPlanCamera.hpp"

#include <gtest/gtest.h>

namespace {

using evp::previewpanel::PlanPointerAction;
using evp::previewpanel::PreviewPlanCamera;

double ModelXAt (const PreviewPlanCamera& camera, double viewportX)
{
    return camera.CenterX () + (viewportX - camera.ViewportWidth () * 0.5) / camera.Scale ();
}

double ModelYAt (const PreviewPlanCamera& camera, double viewportY)
{
    return camera.CenterY () - (viewportY - camera.ViewportHeight () * 0.5) / camera.Scale ();
}

TEST (PreviewPlanCamera, FitsRectangularBoundsWithoutStretching)
{
    PreviewPlanCamera camera;
    camera.SetViewport (400.0, 200.0);
    camera.SetBounds ({ 0.0, 0.0, 200.0, 50.0, true });

    ASSERT_TRUE (camera.Fit (20.0));
    EXPECT_DOUBLE_EQ (camera.CenterX (), 100.0);
    EXPECT_DOUBLE_EQ (camera.CenterY (), 25.0);
    EXPECT_DOUBLE_EQ (camera.Scale (), 1.8);
    const auto transform = camera.Transform ();
    EXPECT_DOUBLE_EQ (transform.scaleX, 1.8);
    EXPECT_DOUBLE_EQ (transform.scaleY, -1.8);
    EXPECT_DOUBLE_EQ (transform.offX, 20.0);
}

TEST (PreviewPlanCamera, WheelZoomKeepsCursorModelAnchorFixed)
{
    PreviewPlanCamera camera;
    camera.SetViewport (500.0, 240.0);
    camera.SetBounds ({ -10.0, -5.0, 30.0, 15.0, true });
    ASSERT_TRUE (camera.Fit ());
    const double anchorX = ModelXAt (camera, 420.0);
    const double anchorY = ModelYAt (camera, 40.0);
    const double beforeScale = camera.Scale ();

    ASSERT_TRUE (camera.ZoomAt (420.0, 40.0, 120));
    EXPECT_GT (camera.Scale (), beforeScale);
    EXPECT_NEAR (ModelXAt (camera, 420.0), anchorX, 1.0e-12);
    EXPECT_NEAR (ModelYAt (camera, 40.0), anchorY, 1.0e-12);
    const double zoomedScale = camera.Scale ();
    ASSERT_TRUE (camera.ZoomAt (420.0, 40.0, -120));
    EXPECT_LT (camera.Scale (), zoomedScale);
}

TEST (PreviewPlanCamera, PanUsesScreenAndModelAxisSigns)
{
    PreviewPlanCamera camera;
    camera.SetViewport (300.0, 200.0);
    camera.SetBounds ({ 0.0, 0.0, 100.0, 100.0, true });
    ASSERT_TRUE (camera.Fit ());
    const double centerX = camera.CenterX ();
    const double centerY = camera.CenterY ();

    ASSERT_TRUE (camera.BeginPan (20.0, 30.0));
    ASSERT_TRUE (camera.PanTo (40.0, 50.0));
    EXPECT_LT (camera.CenterX (), centerX);
    EXPECT_GT (camera.CenterY (), centerY);
}

TEST (PreviewPlanCamera, FitAndWheelRespectScaleClamps)
{
    PreviewPlanCamera camera;
    camera.SetViewport (200.0, 200.0);
    camera.SetBounds ({ 0.0, 0.0, 1.0e20, 1.0e20, true });
    ASSERT_TRUE (camera.Fit ());
    EXPECT_DOUBLE_EQ (camera.Scale (), PreviewPlanCamera::MinimumScale);
    EXPECT_FALSE (camera.ZoomAt (100.0, 100.0, -120));

    camera.SetBounds ({ 0.0, 0.0, 1.0e-20, 1.0e-20, true });
    ASSERT_TRUE (camera.Fit ());
    EXPECT_DOUBLE_EQ (camera.Scale (), PreviewPlanCamera::MaximumScale);
    EXPECT_FALSE (camera.ZoomAt (100.0, 100.0, 120));
}

TEST (PreviewPlanCamera, CaptureContinuesOutsideUntilEndOrCancel)
{
    PreviewPlanCamera camera;
    ASSERT_TRUE (camera.BeginPan (10.0, 10.0));
    EXPECT_TRUE (camera.IsCaptured ());
    EXPECT_TRUE (camera.PanTo (-50.0, 500.0));
    EXPECT_TRUE (camera.EndPan ());
    EXPECT_FALSE (camera.IsCaptured ());
    EXPECT_FALSE (camera.PanTo (0.0, 0.0));

    ASSERT_TRUE (camera.BeginPan (1.0, 2.0));
    EXPECT_TRUE (camera.Cancel ());
    EXPECT_FALSE (camera.IsCaptured ());
}

TEST (PreviewPlanCamera, MiddleDoubleClickRestoresFit)
{
    PreviewPlanCamera camera;
    camera.SetViewport (400.0, 200.0);
    camera.SetBounds ({ 0.0, 0.0, 200.0, 50.0, true });
    ASSERT_TRUE (camera.Fit ());
    const double fittedScale = camera.Scale ();
    ASSERT_TRUE (camera.ZoomAt (300.0, 100.0, 120));

    EXPECT_EQ (camera.MiddleDown (50.0, 60.0, 1000), PlanPointerAction::CaptureStarted);
    EXPECT_TRUE (camera.EndPan ());
    EXPECT_EQ (camera.MiddleDown (50.0, 60.0, 1250), PlanPointerAction::Fitted);
    EXPECT_DOUBLE_EQ (camera.Scale (), fittedScale);
    EXPECT_FALSE (camera.IsCaptured ());
}

TEST (PreviewPlanCamera, ResizePreservesCenterAndScale)
{
    PreviewPlanCamera camera;
    camera.SetViewport (300.0, 200.0);
    camera.SetBounds ({ 0.0, 0.0, 100.0, 50.0, true });
    ASSERT_TRUE (camera.Fit ());
    ASSERT_TRUE (camera.ZoomAt (200.0, 75.0, 120));
    const double centerX = camera.CenterX ();
    const double centerY = camera.CenterY ();
    const double scale = camera.Scale ();

    camera.SetViewport (700.0, 224.0);
    EXPECT_DOUBLE_EQ (camera.CenterX (), centerX);
    EXPECT_DOUBLE_EQ (camera.CenterY (), centerY);
    EXPECT_DOUBLE_EQ (camera.Scale (), scale);
    EXPECT_DOUBLE_EQ (camera.ViewportWidth (), 700.0);
    EXPECT_DOUBLE_EQ (camera.ViewportHeight (), 224.0);
}

} // namespace
