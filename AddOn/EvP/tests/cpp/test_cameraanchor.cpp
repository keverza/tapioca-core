// ArchViz/Dxgi/CameraAnchor: the census's anchor on the centre ray of the camera it decodes.
//
// The cameras are the census's own MATRIX lines from the whole-site run that never locked
// (2026-10-01 13:51:57, archviz.1.log): group g2 at model generation 1, and the same draw at
// generation 10, nine frames into the orbit. The anchor is scored as the census scores it --
// interpretation 0, `b1 * decoded` composed and applied in float -- against the gate's own
// terms: inside the clip volume, area 50 px2, longest edge 10 px, on the run's 3177x1809 view.

#include "ArchViz/Dxgi/CameraAnchor.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace anchor = geomsrv::archviz::dxgi::cameraanchor;
namespace cameralayout = geomsrv::archviz::dxgi::cameralayout;

namespace {

// g2 occ1 idx3513 gen=1 -- the view (b1) and b0, rows as logged.
const float kView1[16] = { 0.6583f, -0.4641f, 0.5927f, 0.0f, 0.7528f,    0.4058f,   -0.5183f,    0.0f,
                           -0.0f,   0.7874f,  0.6165f, 0.0f, -395.5705f, 415.4448f, -1068.7078f, 1.0f };
const float kB01[16] = { 0.8580f, -1.0623f, -0.6169f, -0.5927f, 0.9811f, 0.9290f, 0.5395f,  0.5183f,
                         -0.0f,   1.8024f,  -0.6416f, -0.6165f, 0.0f,    0.0f,    -0.2041f, 0.0f };
// The same draw at gen=10.
const float kView10[16] = { 0.9868f, -0.0802f, 0.1406f, 0.0f, 0.1618f,    0.4890f,   -0.8572f,   0.0f,
                            0.0f,    0.8686f,  0.4955f, 0.0f, -751.8499f, 124.4914f, -765.0596f, 1.0f };
const float kB010[16] = { 1.2862f, -0.1835f, -0.1463f, -0.1406f, 0.2109f, 1.1193f, 0.8922f,  0.8572f,
                          0.0f,    1.9884f,  -0.5157f, -0.4955f, 0.0f,    0.0f,    -0.2041f, 0.0f };

constexpr float kWidth = 3177.0f;
constexpr float kHeight = 1809.0f;

struct Scored {
    bool inside = false;
    float ndcX = 0.0f, ndcY = 0.0f;
    float areaPixels = 0.0f, maxEdgePixels = 0.0f;
};

// `InjectionOracle::ScoreVariants`, interpretation 0, for one triangle: the anchor and the
// two corners `size` along +x and +z.
Scored Score (const float view[16], const float b0[16], const double at[3], double size)
{
    float decoded[16];
    cameralayout::Decode (view, b0, decoded);
    float composed[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += view[r * 4 + k] * decoded[k * 4 + c];
            composed[r * 4 + c] = sum;
        }
    const float corners[3][4] = {
        { float (at[0]), float (at[1]), float (at[2]), 1.0f },
        { float (at[0] + size), float (at[1]), float (at[2]), 1.0f },
        { float (at[0]), float (at[1]), float (at[2] + size), 1.0f },
    };
    float px[3], py[3];
    Scored scored;
    for (int corner = 0; corner < 3; ++corner) {
        float clip[4] = {};
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                clip[c] += corners[corner][r] * composed[r * 4 + c];
        const float x = clip[0] / clip[3];
        const float y = clip[1] / clip[3];
        px[corner] = (x * 0.5f + 0.5f) * kWidth;
        py[corner] = (0.5f - y * 0.5f) * kHeight;
        if (corner == 0) {
            scored.ndcX = x;
            scored.ndcY = y;
            scored.inside = clip[3] > 0.0f && std::fabs (clip[0]) <= clip[3] && std::fabs (clip[1]) <= clip[3] &&
                            clip[2] >= 0.0f && clip[2] <= clip[3];
        }
    }
    const float ax = px[1] - px[0], ay = py[1] - py[0], bx = px[2] - px[0], by = py[2] - py[0];
    scored.areaPixels = std::fabs (ax * by - ay * bx) * 0.5f;
    const float cx = px[2] - px[1], cy = py[2] - py[1];
    scored.maxEdgePixels = std::fmax (std::sqrt (ax * ax + ay * ay),
                                      std::fmax (std::sqrt (bx * bx + by * by), std::sqrt (cx * cx + cy * cy)));
    return scored;
}

void ExpectAdmitted (const Scored& scored)
{
    EXPECT_TRUE (scored.inside);
    EXPECT_NEAR (scored.ndcX, 0.0f, 1e-3f);
    EXPECT_NEAR (scored.ndcY, 0.0f, 1e-3f);
    EXPECT_GE (scored.areaPixels, 50.0f);
    EXPECT_GE (scored.maxEdgePixels, 10.0f);
}

} // namespace

TEST (CameraAnchor, OnTheWholeSiteItIsAtTheCentreOfTheImageAndPassesTheGate)
{
    for (const auto& camera : { std::make_pair (kView1, kB01), std::make_pair (kView10, kB010) }) {
        anchor::Anchor placed;
        ASSERT_TRUE (anchor::OnCentreRay (camera.first, camera.second, nullptr, nullptr, placed));
        EXPECT_DOUBLE_EQ (placed.depth, anchor::kDeepMetres);
        ExpectAdmitted (Score (camera.first, camera.second, placed.at, placed.size));
    }
}

// The run's census scored against points that do not follow the view, and an orbit carried
// them off it. Nine frames into this one the project origin is out of the image; the anchor
// placed on the FIRST frame's centre ray is still in it -- and the runtime places a new one
// every tick while it learns, a frame or two behind, not nine.
TEST (CameraAnchor, AFixedPointLeavesTheViewInAnOrbitTheDeepAnchorDoesNot)
{
    const double origin[3] = { 0.0, 0.0, 0.0 };
    EXPECT_FALSE (Score (kView10, kB010, origin, 50.0).inside);
    anchor::Anchor first;
    ASSERT_TRUE (anchor::OnCentreRay (kView1, kB01, nullptr, nullptr, first));
    const Scored later = Score (kView10, kB010, first.at, first.size);
    EXPECT_TRUE (later.inside);
    EXPECT_LT (std::fabs (later.ndcX), 0.5f);
    EXPECT_LT (std::fabs (later.ndcY), 0.5f);
    // A shallow one does not survive the same orbit: 10 m out, it is off the image.
    double eye[3];
    cameralayout::Eye (kView1, eye);
    const double shallow[3] = { eye[0] + (first.at[0] - eye[0]) * 0.01, eye[1] + (first.at[1] - eye[1]) * 0.01,
                                eye[2] + (first.at[2] - eye[2]) * 0.01 };
    EXPECT_FALSE (Score (kView10, kB010, shallow, 0.5).inside);
}

TEST (CameraAnchor, AModelReachingFartherTakesItPastTheFarthestCorner)
{
    double eye[3];
    cameralayout::Eye (kView1, eye);
    const double boundsMin[3] = { eye[0] - 3000.0, eye[1] - 10.0, eye[2] - 10.0 };
    const double boundsMax[3] = { eye[0] + 10.0, eye[1] + 10.0, eye[2] + 10.0 };
    anchor::Anchor placed;
    ASSERT_TRUE (anchor::OnCentreRay (kView1, kB01, boundsMin, boundsMax, placed));
    EXPECT_NEAR (placed.depth, std::sqrt (3000.0 * 3000.0 + 2 * 10.0 * 10.0), 1e-6);
    ExpectAdmitted (Score (kView1, kB01, placed.at, placed.size));
    // A small model nearer than a kilometre leaves it at a kilometre.
    const double nearMin[3] = { eye[0] - 5.0, eye[1] - 5.0, eye[2] - 5.0 };
    const double nearMax[3] = { eye[0] + 5.0, eye[1] + 5.0, eye[2] + 5.0 };
    ASSERT_TRUE (anchor::OnCentreRay (kView1, kB01, nearMin, nearMax, placed));
    EXPECT_DOUBLE_EQ (placed.depth, anchor::kDeepMetres);
}

TEST (CameraAnchor, AnElementStrandedFarOutCannotPushItPastOurFarPlane)
{
    const double boundsMin[3] = { -1.0e7, -1.0e7, 0.0 };
    const double boundsMax[3] = { 1.0e7, 1.0e7, 10.0 };
    anchor::Anchor placed;
    ASSERT_TRUE (anchor::OnCentreRay (kView1, kB01, boundsMin, boundsMax, placed));
    EXPECT_DOUBLE_EQ (placed.depth, anchor::kDeepestMetres);
    ExpectAdmitted (Score (kView1, kB01, placed.at, placed.size));
}

TEST (CameraAnchor, APairThatIsNotOneCameraPlacesNothing)
{
    // The plan's pixel-to-NDC map (finding 13) as b0: no rotation x projection in it.
    const float screenMap[16] = { 2.0f / 2288.0f, 0, 0, 0, 0, -2.0f / 1202.0f, 0, 0, 0, 0, -1, 0, -1, 1, 0, 1 };
    anchor::Anchor placed;
    EXPECT_FALSE (anchor::OnCentreRay (kView1, screenMap, nullptr, nullptr, placed));
}
