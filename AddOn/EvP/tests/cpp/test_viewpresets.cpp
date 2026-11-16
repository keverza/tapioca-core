// ViewPresets: the standalone viewer's named axonometric views and the projection control's depth
// range (issue #88 M3). The measure is the frame's own view-projection, as in test_camera_plan.cpp:
// what an axis is drawn as, where the model's corners land, and whether they are inside the clip
// volume.

#include "ArchViz/Camera.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/ViewPresets.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace geomsrv::archviz;

namespace {

constexpr float kPi = 3.14159265358979323846f;

struct Clip {
    float px, py, depth;
};

// A world point through view x proj (row vectors): pixels, and the depth the clip test sees (0..1).
Clip Project (const Camera& camera, const float world[3], uint32_t width, uint32_t height)
{
    float view[16], proj[16], viewProj[16];
    camera.GetViewMatrix (view);
    camera.GetProjMatrix (proj, float (width) / float (height));
    Multiply (viewProj, view, proj);
    const float point[4] = { world[0], world[1], world[2], 1.0f };
    float clip[4];
    TransformPoint (clip, point, viewProj);
    Clip out;
    out.px = (clip[0] / clip[3] + 1.0f) * 0.5f * float (width);
    out.py = (1.0f - clip[1] / clip[3]) * 0.5f * float (height);
    out.depth = clip[2] / clip[3];
    return out;
}

void Corner (const float low[3], const float high[3], int k, float out[3])
{
    for (int i = 0; i < 3; ++i)
        out[i] = (k >> i) & 1 ? high[i] : low[i];
}

// Every corner of the box inside the clip volume's depth; `inWindow`, inside the window as well.
void ExpectBoxShown (const Camera& camera, const float low[3], const float high[3], uint32_t width, uint32_t height,
                     bool inWindow, const char* what)
{
    for (int k = 0; k < 8; ++k) {
        float corner[3];
        Corner (low, high, k, corner);
        const Clip c = Project (camera, corner, width, height);
        EXPECT_GE (c.depth, 0.0f) << what << ": corner " << k << " behind the near plane";
        EXPECT_LE (c.depth, 1.0f) << what << ": corner " << k << " past the far plane";
        if (!inWindow)
            continue;
        EXPECT_GE (c.px, 0.0f) << what << ": corner " << k;
        EXPECT_LE (c.px, float (width)) << what << ": corner " << k;
        EXPECT_GE (c.py, 0.0f) << what << ": corner " << k;
        EXPECT_LE (c.py, float (height)) << what << ": corner " << k;
    }
}

// How long a metre along `axis` is drawn, in pixels.
float DrawnLength (const Camera& camera, const float at[3], const float axis[3])
{
    const float tip[3] = { at[0] + axis[0], at[1] + axis[1], at[2] + axis[2] };
    const Clip a = Project (camera, at, 1000, 600);
    const Clip b = Project (camera, tip, 1000, 600);
    return std::hypot (b.px - a.px, b.py - a.py);
}

const float kLow[3] = { 100.0f, -40.0f, -3.0f };
const float kHigh[3] = { 160.0f, -10.0f, 37.0f };

Camera Framed (ViewPreset preset, float north, float aspect = 1000.0f / 600.0f)
{
    Camera camera;
    EXPECT_EQ (ApplyViewPreset (camera, preset, north, false, false, kLow, kHigh, kLow, kHigh, aspect),
               ViewPresetOutcome::Framed);
    return camera;
}

Camera PlanMode (float rotation)
{
    Camera camera;
    camera.SetTarget (120.0f, 40.0f, 3.1f - 5000.0f);
    camera.SetDistance (5000.0f);
    camera.SetTopDown (rotation);
    camera.SetOrthographic (true, 12.0f);
    camera.SetPlanHome (rotation, 3.1f, 12.0f);
    camera.SetOrbitLocked (true);
    return camera;
}

} // namespace

// The issue's table at north = +Y: the eye of "SE" stands south-east of and above the model.
TEST (ViewPresets, TheIsometricCornersAtTheDefaultNorth)
{
    const float third = 1.0f / std::sqrt (3.0f);
    const struct {
        ViewPreset preset;
        float x, y;
    } corners[] = { { ViewPreset::IsometricNE, third, third },
                    { ViewPreset::IsometricNW, -third, third },
                    { ViewPreset::IsometricSE, third, -third },
                    { ViewPreset::IsometricSW, -third, -third } };
    for (const auto& corner : corners) {
        float d[3];
        ASSERT_TRUE (ViewPresetDirection (corner.preset, 90.0f, d));
        EXPECT_NEAR (d[0], corner.x, 1e-6f) << ViewPresetName (corner.preset);
        EXPECT_NEAR (d[1], corner.y, 1e-6f) << ViewPresetName (corner.preset);
        EXPECT_NEAR (d[2], third, 1e-6f) << ViewPresetName (corner.preset);
    }
    float d[3] = { 7.0f, 7.0f, 7.0f };
    EXPECT_FALSE (ViewPresetDirection (ViewPreset::Top, 90.0f, d)) << "the top view is a pose, not a direction";
    EXPECT_FALSE (ViewPresetDirection (ViewPreset::None, 90.0f, d));
    EXPECT_FLOAT_EQ (d[0], 7.0f);
}

// The documented angles: dimetric 19.471 degrees up and 20.705 right of the front, trimetric
// 27.527 up and 11.096 right of the front, isometric 35.264 up.
TEST (ViewPresets, TheDocumentedAngles)
{
    const struct {
        ViewPreset preset;
        float up, rightOfFront;
    } views[] = { { ViewPreset::IsometricSE, 35.2644f, 45.0f },
                  { ViewPreset::Dimetric, 19.4712f, 20.7048f },
                  { ViewPreset::Trimetric, 27.5266f, 11.0961f } };
    for (const auto& view : views) {
        float d[3];
        ASSERT_TRUE (ViewPresetDirection (view.preset, 90.0f, d));
        EXPECT_NEAR (std::asin (d[2]) * 180.0f / kPi, view.up, 1e-3f) << ViewPresetName (view.preset);
        // At north = +Y the front is -Y and the right is +X.
        EXPECT_NEAR (std::atan2 (d[0], -d[1]) * 180.0f / kPi, view.rightOfFront, 1e-3f) << ViewPresetName (view.preset);
    }
}

// ⚠️ NO THREE-QUARTER VIEW UNDER A BORROWED NAME: the names are their foreshortening ratios,
// measured off the picture -- width : height : depth as drawn -- at four norths.
TEST (ViewPresets, TheAxesAreDrawnAtTheNamedRatios)
{
    const struct {
        ViewPreset preset;
        float height, depth; // as fractions of the width's drawn length
    } views[] = { { ViewPreset::IsometricNE, 1.0f, 1.0f },
                  { ViewPreset::IsometricSW, 1.0f, 1.0f },
                  { ViewPreset::Dimetric, 1.0f, 0.5f },
                  { ViewPreset::Trimetric, 0.9f, 0.5f } };
    for (const float north : { 0.0f, 45.0f, 90.0f, 270.0f }) {
        const float n = north * kPi / 180.0f;
        const float widthAxis[3] = { std::sin (n), -std::cos (n), 0.0f }; // the model's right
        const float depthAxis[3] = { std::cos (n), std::sin (n), 0.0f };  // front to back
        const float heightAxis[3] = { 0.0f, 0.0f, 1.0f };
        const float at[3] = { 130.0f, -25.0f, 17.0f };
        for (const auto& view : views) {
            const Camera camera = Framed (view.preset, north);
            EXPECT_TRUE (camera.IsOrthographic ());
            const float width = DrawnLength (camera, at, widthAxis);
            ASSERT_GT (width, 1.0f);
            EXPECT_NEAR (DrawnLength (camera, at, heightAxis) / width, view.height, 2e-3f)
                << ViewPresetName (view.preset) << " north " << north;
            EXPECT_NEAR (DrawnLength (camera, at, depthAxis) / width, view.depth, 2e-3f)
                << ViewPresetName (view.preset) << " north " << north;
        }
    }
}

// The directions turn with project north: a preset at north n is the one at +Y turned by n - 90.
TEST (ViewPresets, TheDirectionsFollowProjectNorth)
{
    for (int k = 1; k < int (ViewPreset::Top); ++k) {
        float base[3];
        ASSERT_TRUE (ViewPresetDirection (ViewPreset (k), 90.0f, base));
        for (const float north : { 0.0f, 45.0f, 90.0f, 270.0f }) {
            const float turn = (north - 90.0f) * kPi / 180.0f;
            float d[3];
            ASSERT_TRUE (ViewPresetDirection (ViewPreset (k), north, d));
            EXPECT_NEAR (d[0], base[0] * std::cos (turn) - base[1] * std::sin (turn), 1e-5f) << k << " " << north;
            EXPECT_NEAR (d[1], base[0] * std::sin (turn) + base[1] * std::cos (turn), 1e-5f) << k << " " << north;
            EXPECT_NEAR (d[2], base[2], 1e-6f);
            EXPECT_NEAR (d[0] * d[0] + d[1] * d[1] + d[2] * d[2], 1.0f, 1e-5f);
        }
    }
}

// A preset is the projection, the direction AND the extent: the whole box in the window and in
// the depth range, in a wide window and in one taller than wide.
TEST (ViewPresets, APresetFramesTheModel)
{
    for (int k = 1; k < int (ViewPreset::Count); ++k) {
        for (const float north : { 0.0f, 45.0f, 90.0f, 270.0f }) {
            const Camera wide = Framed (ViewPreset (k), north);
            EXPECT_TRUE (wide.IsOrthographic ());
            ExpectBoxShown (wide, kLow, kHigh, 1000, 600, true, ViewPresetName (ViewPreset (k)));
            const Camera tall = Framed (ViewPreset (k), north, 400.0f / 900.0f);
            ExpectBoxShown (tall, kLow, kHigh, 400, 900, true, ViewPresetName (ViewPreset (k)));
        }
    }
}

// The eye stands where the name says: the view's direction is the preset's, exactly.
TEST (ViewPresets, TheEyeStandsInTheNamedDirection)
{
    const Camera camera = Framed (ViewPreset::IsometricSE, 90.0f);
    float eye[3], target[3];
    camera.GetEyePosition (eye);
    camera.GetTarget (target);
    const float third = 1.0f / std::sqrt (3.0f);
    EXPECT_NEAR ((eye[0] - target[0]) / camera.Distance (), third, 1e-5f);
    EXPECT_NEAR ((eye[1] - target[1]) / camera.Distance (), -third, 1e-5f);
    EXPECT_NEAR ((eye[2] - target[2]) / camera.Distance (), third, 1e-5f);
    EXPECT_NEAR (target[0], 130.0f, 1e-3f);
    EXPECT_NEAR (target[2], 17.0f, 1e-3f);
}

// ⚠️ THE TOP VIEW HAS PROJECT NORTH UP THE SCREEN, with no degenerate matrix at the pole: a point
// north of the middle is drawn straight above it, and one to the model's right, to the right.
TEST (ViewPresets, TheTopViewHasProjectNorthUp)
{
    for (const float north : { 0.0f, 45.0f, 90.0f, 270.0f }) {
        const Camera camera = Framed (ViewPreset::Top, north);
        EXPECT_TRUE (camera.IsTopDown ());
        EXPECT_TRUE (camera.IsOrthographic ());
        float view[16];
        camera.GetViewMatrix (view);
        for (int k = 0; k < 16; ++k)
            ASSERT_TRUE (std::isfinite (view[k])) << "north " << north;
        const float n = north * kPi / 180.0f;
        const float middle[3] = { 130.0f, -25.0f, 17.0f };
        const float northOf[3] = { middle[0] + 5.0f * std::cos (n), middle[1] + 5.0f * std::sin (n), middle[2] };
        const float rightOf[3] = { middle[0] + 5.0f * std::sin (n), middle[1] - 5.0f * std::cos (n), middle[2] };
        const Clip m = Project (camera, middle, 1000, 600);
        const Clip up = Project (camera, northOf, 1000, 600);
        const Clip right = Project (camera, rightOf, 1000, 600);
        EXPECT_NEAR (up.px, m.px, 0.05f) << "north " << north;
        EXPECT_LT (up.py, m.py - 1.0f) << "north " << north;
        EXPECT_NEAR (right.py, m.py, 0.05f) << "north " << north;
        EXPECT_GT (right.px, m.px + 1.0f) << "north " << north << ": not mirrored";
        float eye[3];
        camera.GetEyePosition (eye);
        EXPECT_GT (eye[2], kHigh[2]) << "over the roof";
    }
}

// With no box known the direction is applied and nothing is invented: the target stays, and the
// extent is the one the perspective showed there.
TEST (ViewPresets, WithNoBoundsADirectionOnly)
{
    Camera camera;
    camera.SetTarget (4.0f, 5.0f, 6.0f);
    camera.SetDistance (20.0f);
    camera.SetOrbit (2.0f, 0.2f);
    EXPECT_EQ (ApplyViewPreset (camera, ViewPreset::IsometricNW, 90.0f, false, false, nullptr, nullptr, nullptr,
                                nullptr, 1.5f),
               ViewPresetOutcome::DirectionOnly);
    EXPECT_NE (ViewPresetNote (ViewPresetOutcome::DirectionOnly), nullptr) << "the HUD says so";
    EXPECT_EQ (ViewPresetNote (ViewPresetOutcome::Framed), nullptr);
    EXPECT_TRUE (camera.IsOrthographic ());
    float target[3];
    camera.GetTarget (target);
    EXPECT_FLOAT_EQ (target[0], 4.0f);
    EXPECT_FLOAT_EQ (target[1], 5.0f);
    EXPECT_FLOAT_EQ (target[2], 6.0f);
    EXPECT_NEAR (camera.OrthoHalfHeightMetres (), 20.0f * std::tan (22.5f * kPi / 180.0f), 1e-4f);
    EXPECT_NEAR (camera.YawDegrees (), 135.0f, 1e-3f);
    EXPECT_NEAR (camera.PitchDegrees (), 35.2644f, 1e-3f);
}

// A preset over a selection frames the selection; "zoom to fit" still means the scene, and the
// scene -- not only the selection -- is inside the depth range.
TEST (ViewPresets, ASelectionIsFramedAndTheSceneStaysTheFit)
{
    const float selLow[3] = { 158.0f, -12.0f, 30.0f }, selHigh[3] = { 160.0f, -10.0f, 33.0f };
    Camera camera;
    ASSERT_EQ (ApplyViewPreset (camera, ViewPreset::Dimetric, 90.0f, false, false, selLow, selHigh, kLow, kHigh,
                                1000.0f / 600.0f),
               ViewPresetOutcome::Framed);
    ExpectBoxShown (camera, selLow, selHigh, 1000, 600, true, "the selection");
    EXPECT_LT (camera.OrthoHalfHeightMetres (), 5.0f) << "framed on the selection, not the scene";
    ExpectBoxShown (camera, kLow, kHigh, 1000, 600, false, "the scene, in depth");
    camera.FrameLastBounds ();
    ExpectBoxShown (camera, kLow, kHigh, 1000, 600, true, "the scene after a fit");
}

// ⚠️ HELD TO THE PLAN, A PRESET DOES NOTHING: the plan stays the held top view.
TEST (ViewPresets, HeldToThePlanNothingChanges)
{
    Camera camera = PlanMode (0.6f);
    float before[16], after[16];
    camera.GetViewMatrix (before);
    for (int k = 1; k < int (ViewPreset::Count); ++k)
        EXPECT_EQ (ApplyViewPreset (camera, ViewPreset (k), 90.0f, true, false, kLow, kHigh, kLow, kHigh, 1.6f),
                   ViewPresetOutcome::HeldToPlan);
    EXPECT_NE (ViewPresetNote (ViewPresetOutcome::HeldToPlan), nullptr);
    camera.GetViewMatrix (after);
    for (int k = 0; k < 16; ++k)
        EXPECT_FLOAT_EQ (after[k], before[k]);
    EXPECT_TRUE (camera.IsTopDown ());
    EXPECT_TRUE (camera.OrbitLocked ());
    EXPECT_NEAR (camera.OrthoHalfHeightMetres (), 12.0f, 1e-6f);
}

// With the orbit freed a preset turns away from the plan and keeps the plan's home; "Top" there is
// the plan's own rotation and cut, not a world top view.
TEST (ViewPresets, FreedThePlanKeepsItsHomeAndTopReturnsToIt)
{
    Camera camera = PlanMode (0.6f);
    camera.SetOrbitLocked (false);
    ASSERT_EQ (ApplyViewPreset (camera, ViewPreset::IsometricSE, 90.0f, true, true, kLow, kHigh, kLow, kHigh, 1.6f),
               ViewPresetOutcome::Framed);
    EXPECT_FALSE (camera.IsTopDown ());
    EXPECT_TRUE (camera.HasPlanHome ());
    ExpectBoxShown (camera, kLow, kHigh, 1000, 600, true, "isometric out of the plan");

    ASSERT_EQ (ApplyViewPreset (camera, ViewPreset::Top, 90.0f, true, true, kLow, kHigh, kLow, kHigh, 1.6f),
               ViewPresetOutcome::PlanHome);
    EXPECT_TRUE (camera.IsTopDown ());
    EXPECT_TRUE (camera.IsOrthographic ());
    EXPECT_NEAR (camera.TopDownRotationRadians (), 0.6f, 1e-6f) << "the plan's rotation, not north's";
    float eye[3];
    camera.GetEyePosition (eye);
    EXPECT_NEAR (eye[2], 3.1f, 1e-3f) << "back on the storey's cut";
}

// ⚠️ A PARALLEL VIEW'S DEPTH IS ITS DISTANCE. Switched to parallel a few metres from a model
// hundreds of metres deep, the far end was past the far plane; the eye backs off, the picture does
// not move, and the way back to perspective is the distance it left with.
TEST (ViewPresets, AProjectionSwitchCloseToALargeModelKeepsItInDepth)
{
    const float low[3] = { -200.0f, -200.0f, 0.0f }, high[3] = { 200.0f, 200.0f, 60.0f };
    Camera camera;
    camera.SetTarget (190.0f, 190.0f, 10.0f);
    camera.SetDistance (5.0f);
    camera.SetOrbit (0.7f, 0.3f);
    camera.SetBounds (low, high);
    EXPECT_FALSE (camera.CoverBoundsInDepth ()) << "perspective has its own far plane";
    EXPECT_FLOAT_EQ (camera.Distance (), 5.0f);

    camera.SwitchProjection (true);
    const float half = camera.OrthoHalfHeightMetres ();
    const float mark[3] = { 191.0f, 189.5f, 10.5f };
    const Clip before = Project (camera, mark, 1000, 600);
    bool clipped = false;
    for (int k = 0; k < 8; ++k) {
        float corner[3];
        Corner (low, high, k, corner);
        const float depth = Project (camera, corner, 1000, 600).depth;
        clipped = clipped || depth < 0.0f || depth > 1.0f;
    }
    ASSERT_TRUE (clipped) << "the case under test: the model is larger than the eye's distance";

    EXPECT_TRUE (camera.CoverBoundsInDepth ());
    ExpectBoxShown (camera, low, high, 1000, 600, false, "after the cover");
    const Clip after = Project (camera, mark, 1000, 600);
    EXPECT_NEAR (after.px, before.px, 0.01f) << "the picture does not move";
    EXPECT_NEAR (after.py, before.py, 0.01f);
    EXPECT_FLOAT_EQ (camera.OrthoHalfHeightMetres (), half);
    EXPECT_FALSE (camera.CoverBoundsInDepth ()) << "once is enough";

    // Turned about its target any way, the model is still inside: the cover is direction-free.
    for (const float yaw : { -2.0f, 0.0f, 1.3f, 3.0f })
        for (const float pitch : { -1.2f, 0.0f, 1.4f }) {
            camera.SetOrbit (yaw, pitch);
            ExpectBoxShown (camera, low, high, 1000, 600, false, "turned");
        }

    camera.SwitchProjection (false);
    EXPECT_NEAR (camera.Distance (), 5.0f, 1e-3f) << "perspective at the size the parallel view showed";
}

// The cover leaves alone what is not its to move: a view with no box, and the plan's own pose,
// whose eye stands on the storey's cut on purpose.
TEST (ViewPresets, TheCoverLeavesThePlanAndABoxlessViewAlone)
{
    Camera none;
    none.SetDistance (2.0f);
    none.SwitchProjection (true);
    EXPECT_FALSE (none.CoverBoundsInDepth ());
    EXPECT_FLOAT_EQ (none.Distance (), 2.0f);

    Camera plan = PlanMode (0.0f);
    const float low[3] = { -9000.0f, -9000.0f, -50.0f }, high[3] = { 9000.0f, 9000.0f, 400.0f };
    plan.SetBounds (low, high);
    EXPECT_FALSE (plan.CoverBoundsInDepth ());
    float eye[3];
    plan.GetEyePosition (eye);
    EXPECT_NEAR (eye[2], 3.1f, 1e-3f);
}

// PLAT-RE142's other half: a preset's parallel view still picks. The ray through a point's pixel
// passes through the point.
TEST (ViewPresets, APresetsParallelViewStillPicks)
{
    for (const ViewPreset preset : { ViewPreset::IsometricSW, ViewPreset::Trimetric, ViewPreset::Top }) {
        const Camera camera = Framed (preset, 45.0f);
        const float world[3] = { 141.0f, -19.0f, 22.0f };
        const Clip at = Project (camera, world, 1000, 600);
        const int32_t px = int32_t (std::floor (at.px)), py = int32_t (std::floor (at.py));
        float origin[3], direction[3];
        camera.CursorRay (px, py, 1000, 600, origin, direction);
        const float to[3] = { world[0] - origin[0], world[1] - origin[1], world[2] - origin[2] };
        const float along = to[0] * direction[0] + to[1] * direction[1] + to[2] * direction[2];
        EXPECT_GT (along, 0.0f) << ViewPresetName (preset) << ": the point is in front of the ray's origin";
        const float off = std::sqrt ((std::max) (0.0f, to[0] * to[0] + to[1] * to[1] + to[2] * to[2] - along * along));
        // Within the pixel the ray was aimed through.
        EXPECT_LT (off, 2.0f * camera.OrthoHalfHeightMetres () / 600.0f * 1.5f) << ViewPresetName (preset);
    }
}
