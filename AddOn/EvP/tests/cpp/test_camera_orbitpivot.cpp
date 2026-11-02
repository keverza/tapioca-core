// Camera: the orbit about a pivot (issue #88, #9) -- the scene's middle, or the selection's. The
// measure is what the user sees: the pivot stays on its pixel while the view turns, in both
// projections and out of the plan's top-down pose, and the picture does not jump when it is set.

#include "ArchViz/Camera.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/OrbitAnchor.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace geomsrv::archviz;

namespace {

constexpr uint32_t kWidth = 1000;
constexpr uint32_t kHeight = 600;

void ToPixel (const Camera& camera, const float world[3], float& px, float& py)
{
    float view[16], proj[16], viewProj[16];
    camera.GetViewMatrix (view);
    camera.GetProjMatrix (proj, float (kWidth) / float (kHeight));
    Multiply (viewProj, view, proj);
    const float point[4] = { world[0], world[1], world[2], 1.0f };
    float clip[4];
    TransformPoint (clip, point, viewProj);
    px = (clip[0] / clip[3] + 1.0f) * 0.5f * float (kWidth) - 0.5f;
    py = (1.0f - clip[1] / clip[3]) * 0.5f * float (kHeight) - 0.5f;
}

InputSnapshot Pointer (int32_t x, int32_t y)
{
    InputSnapshot input;
    input.inside = true;
    input.x = x;
    input.y = y;
    return input;
}

// A Shift drag of the navigation button: an orbit.
void Orbit (Camera& camera, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    InputSnapshot input = Pointer (x0, y0);
    input.shift = true;
    camera.ApplyInput (input, false, kWidth, kHeight);
    input.navButton = true;
    camera.ApplyInput (input, false, kWidth, kHeight);
    input.x = x1;
    input.y = y1;
    camera.ApplyInput (input, false, kWidth, kHeight);
    input.navButton = false;
    camera.ApplyInput (input, false, kWidth, kHeight);
}

// Archicad's kind of start: looking at a point well off the model's middle.
Camera OffCentre ()
{
    Camera camera;
    camera.SetTarget (30.0f, -12.0f, 1.5f);
    camera.SetDistance (25.0f);
    camera.SetOrbit (-0.7f, 0.35f);
    return camera;
}

float Gap (const float a[3], const float b[3])
{
    return std::sqrt ((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

} // namespace

TEST (CameraOrbitPivot, SettingThePivotMovesNothing)
{
    Camera camera = OffCentre ();
    float before[16], after[16];
    camera.GetViewMatrix (before);
    const float pivot[3] = { 0.0f, 0.0f, 4.0f };
    camera.SetOrbitPivot (pivot);
    camera.GetViewMatrix (after);
    for (int i = 0; i < 16; ++i)
        EXPECT_FLOAT_EQ (before[i], after[i]);
}

TEST (CameraOrbitPivot, ThePivotKeepsItsPixelThroughAnOrbit)
{
    for (const bool parallel : { false, true }) {
        Camera camera = OffCentre ();
        if (parallel)
            camera.SwitchProjection (true);
        const float pivot[3] = { 2.0f, 3.0f, 4.0f };
        camera.SetOrbitPivot (pivot);
        float px0, py0;
        ToPixel (camera, pivot, px0, py0);
        float targetBefore[3];
        camera.GetTarget (targetBefore);

        Orbit (camera, 500, 300, 640, 220);

        float px1, py1;
        ToPixel (camera, pivot, px1, py1);
        EXPECT_NEAR (px0, px1, 0.05f) << (parallel ? "parallel" : "perspective");
        EXPECT_NEAR (py0, py1, 0.05f) << (parallel ? "parallel" : "perspective");
        // It turned, and the target went round with it, at its own distance from the pivot.
        float targetAfter[3];
        camera.GetTarget (targetAfter);
        EXPECT_GT (Gap (targetBefore, targetAfter), 1.0f);
        EXPECT_NEAR (Gap (targetBefore, pivot), Gap (targetAfter, pivot), 1e-3f);
        EXPECT_NEAR (camera.Distance (), 25.0f, 1e-4f);
    }
}

TEST (CameraOrbitPivot, WithoutAPivotTheOrbitTurnsAboutTheTarget)
{
    Camera camera = OffCentre ();
    float before[3], after[3];
    camera.GetTarget (before);
    Orbit (camera, 500, 300, 640, 220);
    camera.GetTarget (after);
    EXPECT_FLOAT_EQ (before[0], after[0]);
    EXPECT_FLOAT_EQ (before[1], after[1]);
    EXPECT_FLOAT_EQ (before[2], after[2]);
    camera.SetOrbitPivot (before);
    camera.ClearOrbitPivot ();
    EXPECT_FALSE (camera.HasOrbitPivot ());
}

TEST (CameraOrbitPivot, ThePitchClampDoesNotDriftThePivot)
{
    Camera camera = OffCentre ();
    const float pivot[3] = { -5.0f, 8.0f, 2.0f };
    camera.SetOrbitPivot (pivot);
    float px0, py0;
    ToPixel (camera, pivot, px0, py0);
    // Far past the pole: the clamp holds the pitch, and only the turn applied moves the target.
    Orbit (camera, 500, 100, 500, 100 + 2000);
    EXPECT_LT (camera.PitchDegrees (), 89.5f);
    float px1, py1;
    ToPixel (camera, pivot, px1, py1);
    EXPECT_NEAR (px0, px1, 0.05f);
    EXPECT_NEAR (py0, py1, 0.05f);
}

TEST (CameraOrbitPivot, LeavingThePlansTopDownPoseTurnsAboutThePivot)
{
    Camera camera;
    camera.SetTarget (120.0f, 40.0f, 3.1f - 5000.0f);
    camera.SetDistance (5000.0f);
    camera.SetTopDown (0.6f);
    camera.SetOrthographic (true, 12.0f);
    const float pivot[3] = { 118.0f, 43.0f, 1.5f };
    camera.SetOrbitPivot (pivot);
    float px0, py0;
    ToPixel (camera, pivot, px0, py0);

    Orbit (camera, 500, 300, 560, 330);

    EXPECT_FALSE (camera.IsTopDown ());
    float px1, py1;
    ToPixel (camera, pivot, px1, py1);
    EXPECT_NEAR (px0, px1, 0.05f);
    EXPECT_NEAR (py0, py1, 0.05f);
}

TEST (OrbitAnchorChoice, TheSelectionWinsThenTheSceneThenNothing)
{
    const float selMin[3] = { 1.0f, 2.0f, 3.0f }, selMax[3] = { 3.0f, 4.0f, 5.0f };
    const float sceneMin[3] = { -10.0f, -20.0f, 0.0f }, sceneMax[3] = { 10.0f, 40.0f, 12.0f };

    OrbitAnchor a = ChooseOrbitAnchor (selMin, selMax, sceneMin, sceneMax);
    EXPECT_EQ (a.source, OrbitAnchorSource::Selection);
    EXPECT_FLOAT_EQ (a.point[0], 2.0f);
    EXPECT_FLOAT_EQ (a.point[1], 3.0f);
    EXPECT_FLOAT_EQ (a.point[2], 4.0f);

    a = ChooseOrbitAnchor (nullptr, nullptr, sceneMin, sceneMax);
    EXPECT_EQ (a.source, OrbitAnchorSource::Scene);
    EXPECT_FLOAT_EQ (a.point[0], 0.0f);
    EXPECT_FLOAT_EQ (a.point[1], 10.0f);
    EXPECT_FLOAT_EQ (a.point[2], 6.0f);

    a = ChooseOrbitAnchor (nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ (a.source, OrbitAnchorSource::None);
    EXPECT_STREQ (OrbitAnchorName (a.source), "target");
}

// Archicad's start, its target a metre in front of the eye: the wheel zooms by a fraction of THAT
// distance, so it crawled until a fit. Focused at the model's depth the picture is the same and a
// notch moves the eye a fraction of the way to the model.
TEST (CameraFocusDepth, TheTargetSlidesToTheModelsDepthAndThePictureStays)
{
    Camera camera;
    camera.SetTarget (0.0f, -39.0f, 2.0f);
    camera.SetDistance (1.0f);
    camera.SetOrbit (-0.5f * 3.14159265f, 0.0f); // eye 1 m south of the target, looking north
    float viewBefore[16], eyeBefore[3];
    camera.GetViewMatrix (viewBefore);
    camera.GetEyePosition (eyeBefore);

    const float middle[3] = { 3.0f, 0.0f, 6.0f }; // 40 m north, off the line of sight
    ASSERT_TRUE (camera.FocusAtDepthOf (middle));
    EXPECT_NEAR (camera.Distance (), 40.0f, 1e-3f);

    float viewAfter[16], eyeAfter[3], target[3];
    camera.GetViewMatrix (viewAfter);
    camera.GetEyePosition (eyeAfter);
    camera.GetTarget (target);
    for (int i = 0; i < 16; ++i)
        EXPECT_NEAR (viewBefore[i], viewAfter[i], 1e-3f) << i;
    for (int i = 0; i < 3; ++i)
        EXPECT_NEAR (eyeBefore[i], eyeAfter[i], 1e-3f);
    // On the line of sight, not at the point: the view does not turn to it.
    EXPECT_NEAR (target[0], 0.0f, 1e-3f);
    EXPECT_NEAR (target[1], 0.0f, 1e-3f);
    EXPECT_NEAR (target[2], 2.0f, 1e-3f);
}

TEST (CameraFocusDepth, BehindTheEyeOrParallelNothingChanges)
{
    Camera camera = OffCentre ();
    float eye[3];
    camera.GetEyePosition (eye);
    float target[3];
    camera.GetTarget (target);
    const float behind[3] = { 2.0f * eye[0] - target[0], 2.0f * eye[1] - target[1], 2.0f * eye[2] - target[2] };
    EXPECT_FALSE (camera.FocusAtDepthOf (behind));
    EXPECT_FLOAT_EQ (camera.Distance (), 25.0f);

    camera.SwitchProjection (true);
    const float ahead[3] = { 0.0f, 0.0f, 0.0f };
    EXPECT_FALSE (camera.FocusAtDepthOf (ahead));
    EXPECT_FLOAT_EQ (camera.Distance (), 25.0f);
}
