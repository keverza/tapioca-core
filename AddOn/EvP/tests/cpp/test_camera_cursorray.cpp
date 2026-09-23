// Tests for Camera::CursorRay -- the world ray under a viewport pixel must pass
// through the point the frame's own view-projection draws at that pixel.
//
// ⚠️ THE ONLY CHECK THAT MATTERS IS THE ROUND TRIP. CursorRay rebuilds the
// camera's frame by hand; the frame is drawn through GetViewMatrix x
// GetProjMatrix. If the two disagree -- a mirrored axis, a wrong aspect -- the
// ray is right at the screen centre and wrong everywhere else, which live read
// as "the hover inspector misses objects even in their middle". So: cast the
// ray at a pixel, take a point along it, project that point with the frame's
// matrices, and require the same pixel back -- at the corners, not only the
// centre.

#include <cmath>

#include "ArchViz/Camera.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "gtest/gtest.h"

using namespace geomsrv::archviz;

namespace {

constexpr uint32_t kWidth = 1000;
constexpr uint32_t kHeight = 600;

// Project a world point to viewport pixels through view x proj, the way the
// frame does (row vectors, depth 0..1).
void ToPixel (const Camera& camera, const float world[3], float& px, float& py)
{
    float view[16];
    float proj[16];
    float viewProj[16];
    camera.GetViewMatrix (view);
    camera.GetProjMatrix (proj, float (kWidth) / float (kHeight));
    Multiply (viewProj, view, proj);
    const float point[4] = { world[0], world[1], world[2], 1.0f };
    float clip[4];
    TransformPoint (clip, point, viewProj);
    const float ndcX = clip[0] / clip[3];
    const float ndcY = clip[1] / clip[3];
    // The inverse of CursorRay's own pixel-centre convention.
    px = (ndcX + 1.0f) * 0.5f * float (kWidth) - 0.5f;
    py = (1.0f - ndcY) * 0.5f * float (kHeight) - 0.5f;
}

void ExpectRoundTrip (const Camera& camera, const char* what)
{
    const int32_t pixels[5][2] = { { 500, 300 }, { 40, 30 }, { 960, 40 }, { 60, 570 }, { 930, 560 } };
    for (const auto& pixel : pixels) {
        float origin[3];
        float direction[3];
        camera.CursorRay (pixel[0], pixel[1], kWidth, kHeight, origin, direction);
        const float world[3] = { origin[0] + direction[0] * 25.0f, origin[1] + direction[1] * 25.0f,
                                 origin[2] + direction[2] * 25.0f };
        float px = 0.0f;
        float py = 0.0f;
        ToPixel (camera, world, px, py);
        EXPECT_NEAR (px, float (pixel[0]), 0.05f) << what << ": pixel " << pixel[0] << ", " << pixel[1];
        EXPECT_NEAR (py, float (pixel[1]), 0.05f) << what << ": pixel " << pixel[0] << ", " << pixel[1];
    }
}

} // namespace

TEST (CameraCursorRay, APerspectiveRayPassesThroughItsPixelAtEveryCorner)
{
    Camera camera;
    camera.SetTarget (10.0f, 20.0f, 3.0f);
    camera.SetDistance (40.0f);
    camera.SetFovDegreesVertical (45.0f);
    const float yaws[3] = { 0.3f, 1.9f, 4.0f };
    for (const float yaw : yaws) {
        camera.SetOrbit (yaw, 0.6f);
        ExpectRoundTrip (camera, "perspective");
    }
}

TEST (CameraCursorRay, AnOrthographicRayPassesThroughItsPixelAtEveryCorner)
{
    Camera camera;
    camera.SetTarget (-5.0f, 8.0f, 0.0f);
    camera.SetDistance (60.0f);
    camera.SetOrbit (2.2f, 0.7f);
    camera.SetOrthographic (true, 15.0f);
    ExpectRoundTrip (camera, "orthographic");
}
