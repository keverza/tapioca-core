// Camera: navigating the top-down parallel pose by hand -- the viewer opened in the plan's
// place (the user, 2026-10-03: the plan view was empty; it must be navigable and come back
// to the top view). Every check goes through the frame's own view-projection, as
// test_camera_cursorray.cpp's do: what the user sees under the cursor is the measure.

#include "ArchViz/Camera.hpp"
#include "ArchViz/MatrixMath.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace geomsrv::archviz;

namespace {

constexpr uint32_t kWidth = 1000;
constexpr uint32_t kHeight = 600;

// A world point to viewport pixels through view x proj (row vectors), pixel centres.
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

// The plan's pose as the viewer opens it: straight down, turned `rotation`, parallel, the eye
// at 3.1 m above a storey at 2 m.
Camera Plan (float rotation)
{
    Camera camera;
    camera.SetTarget (120.0f, 40.0f, 3.1f - 5000.0f);
    camera.SetDistance (5000.0f);
    camera.SetTopDown (rotation);
    camera.SetOrthographic (true, 12.0f);
    return camera;
}

InputSnapshot Pointer (int32_t x, int32_t y)
{
    InputSnapshot input;
    input.inside = true;
    input.x = x;
    input.y = y;
    return input;
}

// A drag of the navigation button from one pixel to another, Shift held or not.
void Drag (Camera& camera, int32_t x0, int32_t y0, int32_t x1, int32_t y1, bool shift)
{
    InputSnapshot input = Pointer (x0, y0);
    input.shift = shift;
    camera.ApplyInput (input, false, kWidth, kHeight); // the pointer there
    input.navButton = true;
    camera.ApplyInput (input, false, kWidth, kHeight); // pressed
    input.x = x1;
    input.y = y1;
    camera.ApplyInput (input, false, kWidth, kHeight); // dragged
    input.navButton = false;
    camera.ApplyInput (input, false, kWidth, kHeight); // let go
}

float Eye (const Camera& camera)
{
    float eye[3];
    camera.GetEyePosition (eye);
    return eye[2];
}

} // namespace

// ⚠️ A PAN IN THE PLAN POSE CARRIES THE MODEL WITH THE CURSOR, at any rotation of the plan: its
// axes were a quarter turn off, and a drag to the right moved the plan up.
TEST (CameraPlan, APanCarriesThePlanWithTheCursor)
{
    for (const float rotation : { 0.0f, 0.6f, -2.2f }) {
        Camera camera = Plan (rotation);
        const float world[3] = { 123.0f, 37.0f, 2.0f }; // a point on the storey's floor
        float x0 = 0.0f, y0 = 0.0f;
        ToPixel (camera, world, x0, y0);
        Drag (camera, int32_t (std::lround (x0)), int32_t (std::lround (y0)), int32_t (std::lround (x0)) + 120,
              int32_t (std::lround (y0)) + 60, false);
        float x1 = 0.0f, y1 = 0.0f;
        ToPixel (camera, world, x1, y1);
        EXPECT_NEAR (x1 - x0, 120.0f, 0.5f) << "rotation " << rotation;
        EXPECT_NEAR (y1 - y0, 60.0f, 0.5f) << "rotation " << rotation;
        EXPECT_NEAR (Eye (camera), 3.1f, 1e-3f) << "the eye stays at the cut";
    }
}

// ⚠️ THE WHEEL ZOOMS A PARALLEL VIEW BY ITS EXTENT, about the cursor, and the eye stays where it
// is: it used to move the eye -- nothing zoomed, and the eye sank through the storey's cut.
TEST (CameraPlan, TheWheelZoomsAParallelViewAboutTheCursorAndTheEyeStays)
{
    Camera camera = Plan (0.6f);
    // A floor point off the view's middle, and the cursor on it.
    const float world[3] = { 126.0f, 36.0f, 2.0f };
    float x0 = 0.0f, y0 = 0.0f;
    ToPixel (camera, world, x0, y0);
    InputSnapshot wheel = Pointer (int32_t (std::lround (x0)), int32_t (std::lround (y0)));
    wheel.wheelDelta = 120; // a notch towards the model: closer
    EXPECT_TRUE (camera.ApplyInput (wheel, false, kWidth, kHeight));
    EXPECT_NEAR (camera.OrthoHalfHeightMetres (), 12.0f * 0.88f, 1e-4f) << "the extent zoomed";
    EXPECT_NEAR (Eye (camera), 3.1f, 1e-3f) << "the eye at the cut";
    float px = 0.0f, py = 0.0f;
    ToPixel (camera, world, px, py);
    // About the middle it would have moved some 25 px.
    EXPECT_NEAR (px, x0, 0.3f) << "the point under the cursor stays under it";
    EXPECT_NEAR (py, y0, 0.3f);
    // Out again, and far past the limit: held at it.
    wheel.wheelDelta = -120 * 400;
    camera.ApplyInput (wheel, false, kWidth, kHeight);
    EXPECT_LE (camera.OrthoHalfHeightMetres (), 100000.0f);
    EXPECT_NEAR (Eye (camera), 3.1f, 1e-3f);
}

// An orbit begun in the plan pose leaves it from where the picture is: the first pixel of the
// drag barely moves the model -- not a quarter turn of it.
TEST (CameraPlan, AnOrbitLeavesThePlanPoseFromWhereItIs)
{
    Camera camera = Plan (0.6f);
    const float world[3] = { 126.0f, 44.0f, 3.1f - 5000.0f + 0.0f };
    float x0 = 0.0f, y0 = 0.0f;
    ToPixel (camera, world, x0, y0);
    Drag (camera, 500, 300, 501, 300, true);
    float x1 = 0.0f, y1 = 0.0f;
    ToPixel (camera, world, x1, y1);
    EXPECT_NEAR (x1, x0, 4.0f);
    EXPECT_NEAR (y1, y0, 4.0f);
    EXPECT_LT (camera.PitchDegrees (), 90.0f) << "an ordinary orbit now";

    // ⚠️ AND IT IS THE ORBIT'S VIEW, NOT THE PLAN'S ROLL ON AN ORBITING EYE: the same view an orbit
    // set to that yaw and pitch has.
    Drag (camera, 500, 300, 560, 420, true);
    constexpr float kDegrees = 3.14159265358979f / 180.0f;
    Camera orbit;
    float target[3];
    camera.GetTarget (target);
    orbit.SetTarget (target[0], target[1], target[2]);
    orbit.SetDistance (camera.Distance ());
    orbit.SetOrbit (camera.YawDegrees () * kDegrees, camera.PitchDegrees () * kDegrees);
    float mine[16], theirs[16];
    camera.GetViewMatrix (mine);
    orbit.GetViewMatrix (theirs);
    for (int k = 0; k < 16; ++k)
        EXPECT_NEAR (mine[k], theirs[k], 2e-3f) << "element " << k;
}
