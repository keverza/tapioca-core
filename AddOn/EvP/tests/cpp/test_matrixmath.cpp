// ArchViz/MatrixMath — the camera's conventions, checked as arithmetic.
//
// ⚠️ WHY OFFLINE: every failure mode here renders a PLAUSIBLE picture. A
// left-handed view matrix over Archicad's right-handed world mirrors the
// building — north and south swapped — and a debug cube survives that looking
// perfectly fine; it shipped in this repo once and was found only by measuring
// which way pan and orbit went. A Y-up "fix" is the same class of bug. A
// homogeneous-depth projection inverts the depth test over half the scene.
// None of that is visible by opening Archicad and looking, so none of it should
// be checked that way.
//
// The expected values are worked out from bx's `src/math.cpp` (mtxLookAt,
// mtxProjXYWH) BY HAND, not by calling bx: the point is that the port produces
// the same matrices bgfx did, and a test that called the thing under test's own
// source of truth would prove nothing.

#include "ArchViz/MatrixMath.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace geomsrv::archviz;

namespace {

constexpr float kEps = 1e-5f;

// Archicad's axes, spelled out so the tests read as geometry and not as indices.
constexpr float kUpZ[3] = {0.0f, 0.0f, 1.0f};
constexpr float kOrigin[3] = {0.0f, 0.0f, 0.0f};

// A camera due SOUTH of the origin at eye level, looking north at it. This is
// the orientation the mirror bug is visible in: east must land on the RIGHT.
constexpr float kEyeSouth[3] = {0.0f, -10.0f, 0.0f};

void Project (float outNdc[3], const float world[3], const float viewProj[16])
{
    const float v[4] = {world[0], world[1], world[2], 1.0f};
    float clip[4];
    TransformPoint (clip, v, viewProj);
    ASSERT_GT (clip[3], 0.0f) << "point is behind the camera";
    outNdc[0] = clip[0] / clip[3];
    outNdc[1] = clip[1] / clip[3];
    outNdc[2] = clip[2] / clip[3];
}

void MakeViewProj (float out[16], const float eye[3], float aspect = 1.0f,
                   float fovY = 45.0f, float nearZ = 0.05f, float farZ = 20000.0f)
{
    float view[16];
    float proj[16];
    LookAtRH (view, eye, kOrigin, kUpZ);
    PerspectiveRH (proj, fovY, aspect, nearZ, farZ);
    Multiply (out, view, proj);
}

}   // namespace

// --- the handedness / mirror guard -----------------------------------------

// THE test. Camera south of the model looking north, Z-up: east is on the
// right of the screen and up is up. A mirrored (left-handed) pair passes the
// vertical half of this and fails the horizontal half — which is exactly the
// asymmetry the 2026-08-06 measurement reported.
TEST (MatrixMath, EastIsScreenRightAndUpIsScreenUp)
{
    float viewProj[16];
    MakeViewProj (viewProj, kEyeSouth);

    const float east[3] = {1.0f, 0.0f, 0.0f};
    const float west[3] = {-1.0f, 0.0f, 0.0f};
    const float up[3] = {0.0f, 0.0f, 1.0f};
    const float down[3] = {0.0f, 0.0f, -1.0f};

    float ndc[3];
    Project (ndc, east, viewProj);
    EXPECT_GT (ndc[0], 0.0f) << "east must be screen-right; the image is mirrored";
    Project (ndc, west, viewProj);
    EXPECT_LT (ndc[0], 0.0f);
    Project (ndc, up, viewProj);
    EXPECT_GT (ndc[1], 0.0f) << "world +Z must be screen-up; this is the Z-up decision";
    Project (ndc, down, viewProj);
    EXPECT_LT (ndc[1], 0.0f);
}

// Same check from a second station, because a mirror about one axis can look
// correct from the station that happens to be on that axis.
TEST (MatrixMath, HandednessHoldsFromTheWest)
{
    const float eyeWest[3] = {-10.0f, 0.0f, 0.0f};
    float viewProj[16];
    MakeViewProj (viewProj, eyeWest);

    // Looking east: NORTH is now on the left of the screen.
    const float north[3] = {0.0f, 1.0f, 0.0f};
    float ndc[3];
    Project (ndc, north, viewProj);
    EXPECT_LT (ndc[0], 0.0f) << "looking east, north is screen-left";
}

// --- LookAtRH element by element -------------------------------------------

TEST (MatrixMath, LookAtBasisMatchesTheHandComputedRows)
{
    float view[16];
    LookAtRH (view, kEyeSouth, kOrigin, kUpZ);

    // view axis = normalize(eye - at) = (0,-1,0)
    // right     = normalize(cross(up, view)) = cross((0,0,1),(0,-1,0)) = (1,0,0)
    // trueUp    = cross(view, right) = (0,0,1)
    EXPECT_NEAR (view[0], 1.0f, kEps);   // right.x
    EXPECT_NEAR (view[1], 0.0f, kEps);   // trueUp.x
    EXPECT_NEAR (view[2], 0.0f, kEps);   // view.x
    EXPECT_NEAR (view[4], 0.0f, kEps);   // right.y
    EXPECT_NEAR (view[5], 0.0f, kEps);   // trueUp.y
    EXPECT_NEAR (view[6], -1.0f, kEps);  // view.y
    EXPECT_NEAR (view[8], 0.0f, kEps);   // right.z
    EXPECT_NEAR (view[9], 1.0f, kEps);   // trueUp.z
    EXPECT_NEAR (view[10], 0.0f, kEps);  // view.z
    // translation = -dot(axis, eye)
    EXPECT_NEAR (view[12], 0.0f, kEps);
    EXPECT_NEAR (view[13], 0.0f, kEps);
    EXPECT_NEAR (view[14], -10.0f, kEps);
    EXPECT_NEAR (view[15], 1.0f, kEps);
}

// Right-handed means the scene sits at NEGATIVE view-space z. If this flips,
// the projection's -1 in [11] produces a negative w and everything clips away.
TEST (MatrixMath, TheSceneLandsAtNegativeViewZ)
{
    float view[16];
    LookAtRH (view, kEyeSouth, kOrigin, kUpZ);

    const float atOrigin[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float camera[4];
    TransformPoint (camera, atOrigin, view);
    EXPECT_NEAR (camera[2], -10.0f, kEps);
    EXPECT_NEAR (camera[0], 0.0f, kEps);
    EXPECT_NEAR (camera[1], 0.0f, kEps);
}

// A plan view -- straight down, up parallel to the line of sight -- is a normal
// thing for an architectural viewer to do and a degenerate cross product. bx
// falls back to +X; without the fallback every element is NaN and the viewport
// goes empty with no error anywhere.
TEST (MatrixMath, StraightDownDoesNotProduceNaNs)
{
    const float eyeAbove[3] = {0.0f, 0.0f, 30.0f};
    float view[16];
    LookAtRH (view, eyeAbove, kOrigin, kUpZ);
    for (int i = 0; i < 16; ++i)
        EXPECT_FALSE (std::isnan (view[i])) << "element " << i;
    EXPECT_NEAR (view[0], 1.0f, kEps) << "the +X fallback basis";
}

// --- PerspectiveRH ----------------------------------------------------------

TEST (MatrixMath, ProjectionElementsMatchBxsNonHomogeneousForm)
{
    constexpr float kFov = 45.0f;
    constexpr float kAspect = 16.0f / 9.0f;
    constexpr float kNear = 0.05f;
    constexpr float kFar = 100.0f;

    float proj[16];
    PerspectiveRH (proj, kFov, kAspect, kNear, kFar);

    const float height = 1.0f / std::tan (kFov * (3.14159265358979323846f / 180.0f) * 0.5f);
    const float width = height / kAspect;
    const float aa = kFar / (kFar - kNear);

    EXPECT_NEAR (proj[0], width, kEps);
    EXPECT_NEAR (proj[5], height, kEps);
    EXPECT_NEAR (proj[10], -aa, kEps);
    EXPECT_NEAR (proj[11], -1.0f, kEps);
    EXPECT_NEAR (proj[14], -kNear * aa, kEps);
    EXPECT_NEAR (proj[15], 0.0f, kEps) << "a perspective matrix is not affine";
    // Everything else is zero.
    const int nonZero[6] = {0, 5, 10, 11, 14, 15};
    for (int i = 0; i < 16; ++i) {
        bool listed = false;
        for (int j = 0; j < 6; ++j)
            listed = listed || nonZero[j] == i;
        if (!listed)
            EXPECT_NEAR (proj[i], 0.0f, kEps) << "element " << i;
    }
}

// Depth 0..1, NOT -1..1. The homogeneous form would put the near plane at -1,
// and half the scene would fail the depth test rather than the whole thing
// failing visibly.
TEST (MatrixMath, NearMapsToZeroAndFarToOne)
{
    constexpr float kNear = 0.5f;
    constexpr float kFar = 250.0f;
    float proj[16];
    PerspectiveRH (proj, 45.0f, 1.0f, kNear, kFar);

    // A point on the near plane sits at view-space z = -near.
    const float onNear[4] = {0.0f, 0.0f, -kNear, 1.0f};
    const float onFar[4] = {0.0f, 0.0f, -kFar, 1.0f};
    float clip[4];

    TransformPoint (clip, onNear, proj);
    EXPECT_NEAR (clip[3], kNear, kEps);
    EXPECT_NEAR (clip[2] / clip[3], 0.0f, 1e-4f);

    TransformPoint (clip, onFar, proj);
    EXPECT_NEAR (clip[3], kFar, kEps);
    EXPECT_NEAR (clip[2] / clip[3], 1.0f, 1e-4f);
}

TEST (MatrixMath, AspectSquashesHorizontallyNotVertically)
{
    float wide[16];
    float square[16];
    PerspectiveRH (wide, 45.0f, 2.0f, 0.05f, 100.0f);
    PerspectiveRH (square, 45.0f, 1.0f, 0.05f, 100.0f);
    // The VERTICAL field of view is the one held fixed (the header says so).
    EXPECT_NEAR (wide[5], square[5], kEps);
    EXPECT_NEAR (wide[0], square[0] * 0.5f, kEps);
}

// --- Multiply / TransformPoint ---------------------------------------------

TEST (MatrixMath, MultiplyIsViewThenProjection)
{
    float view[16];
    float proj[16];
    LookAtRH (view, kEyeSouth, kOrigin, kUpZ);
    PerspectiveRH (proj, 45.0f, 1.0f, 0.05f, 100.0f);

    float viewProj[16];
    Multiply (viewProj, view, proj);

    const float world[4] = {1.0f, 2.0f, 3.0f, 1.0f};
    float throughBoth[4];
    float camera[4];
    float stepwise[4];
    TransformPoint (throughBoth, world, viewProj);
    TransformPoint (camera, world, view);
    TransformPoint (stepwise, camera, proj);
    for (int i = 0; i < 4; ++i)
        EXPECT_NEAR (throughBoth[i], stepwise[i], 1e-4f) << "component " << i;
}

TEST (MatrixMath, MultiplyMayAliasItsOutput)
{
    float a[16];
    float b[16];
    LookAtRH (a, kEyeSouth, kOrigin, kUpZ);
    PerspectiveRH (b, 45.0f, 1.0f, 0.05f, 100.0f);

    float expected[16];
    Multiply (expected, a, b);
    Multiply (a, a, b);   // in place
    for (int i = 0; i < 16; ++i)
        EXPECT_NEAR (a[i], expected[i], kEps) << "element " << i;
}

TEST (MatrixMath, TransformPointMayAliasItsOutput)
{
    float m[16];
    PerspectiveRH (m, 45.0f, 1.0f, 0.05f, 100.0f);
    float v[4] = {1.0f, 2.0f, -5.0f, 1.0f};
    float expected[4];
    TransformPoint (expected, v, m);
    TransformPoint (v, v, m);
    for (int i = 0; i < 4; ++i)
        EXPECT_NEAR (v[i], expected[i], kEps) << "component " << i;
}

// The identity is the one matrix whose product is checkable by inspection, and
// it catches a row/column transposition that the geometric tests above would
// only catch for asymmetric inputs.
TEST (MatrixMath, MultiplyByIdentityIsIdentityOnBoth)
{
    float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float view[16];
    LookAtRH (view, kEyeSouth, kOrigin, kUpZ);

    float left[16];
    float right[16];
    Multiply (left, identity, view);
    Multiply (right, view, identity);
    for (int i = 0; i < 16; ++i) {
        EXPECT_NEAR (left[i], view[i], kEps) << "element " << i;
        EXPECT_NEAR (right[i], view[i], kEps) << "element " << i;
    }
}

// ---- OrthographicRH — the sun's projection ---------------------------------
//
// Checked as behaviour rather than element by element: an orthographic matrix
// whose elements are individually plausible can still map the near plane to 1
// and the far plane to 0, and that inversion shadows everything except what
// should be in shadow.

TEST (MatrixMath, OrthographicMapsTheNearAndFarPlanesToZeroAndOne)
{
    float proj[16];
    OrthographicRH (proj, -10.0f, 10.0f, -10.0f, 10.0f, 2.0f, 42.0f);

    // ⚠️ NEGATIVE VIEW-SPACE z. nearZ and farZ are DISTANCES; a right-handed
    // camera looks down -z, so the near plane is the point at z = -nearZ.
    const float atNear[4] = {0.0f, 0.0f, -2.0f, 1.0f};
    const float atFar[4] = {0.0f, 0.0f, -42.0f, 1.0f};
    float clipNear[4];
    float clipFar[4];
    TransformPoint (clipNear, atNear, proj);
    TransformPoint (clipFar, atFar, proj);

    EXPECT_NEAR (clipNear[2] / clipNear[3], 0.0f, kEps);
    EXPECT_NEAR (clipFar[2] / clipFar[3], 1.0f, kEps);
    // Affine: w is untouched. This is the whole difference from PerspectiveRH,
    // whose w is -z, and it is why an ortho projection does not foreshorten.
    EXPECT_NEAR (clipNear[3], 1.0f, kEps);
    EXPECT_NEAR (clipFar[3], 1.0f, kEps);
}

TEST (MatrixMath, OrthographicMapsTheBoxEdgesToTheClipEdges)
{
    // An OFF-CENTRE box, which is the case that catches a missing translation
    // term: a centred box passes with out[12] and out[13] both zero.
    float proj[16];
    OrthographicRH (proj, 4.0f, 14.0f, -6.0f, 2.0f, 1.0f, 11.0f);

    const float leftBottom[4] = {4.0f, -6.0f, -6.0f, 1.0f};
    const float rightTop[4] = {14.0f, 2.0f, -6.0f, 1.0f};
    float a[4];
    float b[4];
    TransformPoint (a, leftBottom, proj);
    TransformPoint (b, rightTop, proj);

    EXPECT_NEAR (a[0], -1.0f, kEps);
    EXPECT_NEAR (a[1], -1.0f, kEps);
    EXPECT_NEAR (b[0], 1.0f, kEps);
    EXPECT_NEAR (b[1], 1.0f, kEps);
}

TEST (MatrixMath, OrthographicSurvivesADegenerateBox)
{
    // A zero-width box is what an empty scene produces, and a division by zero
    // here yields a matrix of NaN -- which renders as nothing at all rather than
    // as an error.
    float proj[16];
    OrthographicRH (proj, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 16; ++i)
        EXPECT_TRUE (std::isfinite (proj[i])) << "element " << i;
}

// ---------------------------------------------------------------------------
// CameraBasis — the frame shared by DiligentPickBuffer::Aim and
// Camera::CursorRay (PLAT-RE62). It is tested here rather than in either caller
// because the whole reason it exists is that there must be exactly ONE of it.
// ---------------------------------------------------------------------------

TEST (MatrixMath, CameraBasisIsOrthonormalAndRightHanded)
{
    const float eye[3] = {10.0f, -4.0f, 6.0f};
    const float target[3] = {1.0f, 2.0f, 1.5f};
    float forward[3];
    float right[3];
    float up[3];
    CameraBasis (eye, target, forward, right, up);

    const auto dot = [] (const float a[3], const float b[3]) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };

    EXPECT_NEAR (dot (forward, forward), 1.0f, kEps);
    EXPECT_NEAR (dot (right, right), 1.0f, kEps);
    EXPECT_NEAR (dot (up, up), 1.0f, kEps);

    EXPECT_NEAR (dot (forward, right), 0.0f, kEps);
    EXPECT_NEAR (dot (forward, up), 0.0f, kEps);
    EXPECT_NEAR (dot (right, up), 0.0f, kEps);

    // `forward` really points from the eye at the target.
    float toTarget[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    const float len = std::sqrt (dot (toTarget, toTarget));
    for (int k = 0; k < 3; ++k)
        EXPECT_NEAR (forward[k], toTarget[k] / len, kEps);

    // ⚠️ `right` MUST BE HORIZONTAL in a Z-up world: it is cross(worldUp,
    // forward), so it has no z component whatever the pitch. A screen-space
    // offset built on a right with a z term slides the pick diagonally as the
    // camera tilts, which is the kind of error that looks like bad aim.
    EXPECT_NEAR (right[2], 0.0f, kEps);
}

// ⚠️ CameraBasis's `right` IS SCREEN-*LEFT*, AND THIS TEST EXISTS BECAUSE THAT
// COST A LIVE ROUND TRIP. It is cross(worldUp, forward); the view matrix's
// screen-right row is cross(up, eye - at) = cross(up, -forward), which is the
// NEGATIVE of it. Both are "right" in their own frame and neither is wrong, but
// anything that maps NDC +x onto a world direction -- the sky background's view
// ray (DiligentViewport) is the first -- must negate it or the picture comes out
// horizontally MIRRORED. Mirrored is the worst kind of wrong here: the image
// still looks like a sky, and the only visible symptom is that it slides the
// wrong way when the camera turns.
TEST (MatrixMath, CameraBasisRightIsTheNegativeOfTheViewMatrixScreenRight)
{
    const float eye[3] = {10.0f, -4.0f, 6.0f};
    const float target[3] = {1.0f, 2.0f, 1.5f};
    float forward[3];
    float right[3];
    float up[3];
    CameraBasis (eye, target, forward, right, up);

    const float worldUp[3] = {0.0f, 0.0f, 1.0f};
    float view[16];
    LookAtRH (view, eye, target, worldUp);

    // Column 0 of the stored matrix is the screen-right axis -- see LookAtRH's
    // element assignment, where right[k] lands in out[4*k].
    const float screenRight[3] = {view[0], view[4], view[8]};
    for (int k = 0; k < 3; ++k)
        EXPECT_NEAR (screenRight[k], -right[k], kEps);

    // And the two `up`s AGREE, so only the horizontal axis needs the flip. A
    // reader who negates both gets a 180-degree rotation instead of a mirror,
    // which is a different and equally silent bug.
    const float screenUp[3] = {view[1], view[5], view[9]};
    for (int k = 0; k < 3; ++k)
        EXPECT_NEAR (screenUp[k], up[k], kEps);
}

TEST (MatrixMath, CameraBasisSurvivesLookingStraightDown)
{
    // The degenerate case the plan overlay's top-down camera sits in every
    // frame: forward is parallel to world up, so cross(worldUp, forward)
    // vanishes and any perpendicular frame will do -- but it must be FINITE.
    const float eye[3] = {0.0f, 0.0f, 50.0f};
    const float target[3] = {0.0f, 0.0f, 0.0f};
    float forward[3];
    float right[3];
    float up[3];
    CameraBasis (eye, target, forward, right, up);

    for (int k = 0; k < 3; ++k) {
        EXPECT_TRUE (std::isfinite (forward[k])) << "forward " << k;
        EXPECT_TRUE (std::isfinite (right[k])) << "right " << k;
        EXPECT_TRUE (std::isfinite (up[k])) << "up " << k;
    }
    EXPECT_NEAR (forward[2], -1.0f, kEps);

    const auto dot = [] (const float a[3], const float b[3]) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    EXPECT_NEAR (dot (right, right), 1.0f, kEps);
    EXPECT_NEAR (dot (forward, right), 0.0f, kEps);
}

TEST (MatrixMath, CameraBasisSurvivesAZeroLengthView)
{
    // Eye and target coincident -- what an uninitialised camera holds. A NaN
    // basis here propagates into every matrix built from it.
    const float eye[3] = {3.0f, 3.0f, 3.0f};
    float forward[3];
    float right[3];
    float up[3];
    CameraBasis (eye, eye, forward, right, up);

    for (int k = 0; k < 3; ++k) {
        EXPECT_TRUE (std::isfinite (forward[k])) << "forward " << k;
        EXPECT_TRUE (std::isfinite (right[k])) << "right " << k;
        EXPECT_TRUE (std::isfinite (up[k])) << "up " << k;
    }
}

// ---- the left-handed pair DiligentFX's post-processing insists on -----------
//
// ⚠️ OFFLINE FOR THE SAME REASON AS EVERYTHING ABOVE: this failure renders a
// plausible picture too. Handing DiligentFX the right-handed pair does not
// produce black or NaN, it produces reflections that are THERE and wrong --
// mirrored about the view axis and with every hit accepted, because the
// thickness rejection divides by a view-space z that has the wrong sign. It
// shipped that way and was found by re-deriving the shader's algebra, not by
// looking. The two assertions below are exactly what that derivation needs to
// hold: that the depth buffer is unaffected, and that the shader's own
// reconstruction comes back to where it started.

namespace {

// PostFX_Common.fxh's ScreenXYDepthToViewSpace, transcribed. `Transform` is
// indexed the way MATRIX_ELEMENT does under row-major packing: element (r, c)
// is m[r * 4 + c].
void ScreenXYDepthToViewSpace (float out[3], const float uv[2], float depth, const float proj[16])
{
    const float m22 = proj[2 * 4 + 2];
    const float m32 = proj[3 * 4 + 2];
    const float m23 = proj[2 * 4 + 3];
    const float m33 = proj[3 * 4 + 3];
    // NormalizedDeviceZToCameraZ. D3D depth IS ndc z, so no remap first.
    const float cameraZ = (m32 - depth * m33) / (depth * m23 - m22);

    const float ndcX = uv[0] * 2.0f - 1.0f;
    const float ndcY = -(uv[1] * 2.0f - 1.0f);
    out[0] = cameraZ * ndcX / proj[0];
    out[1] = cameraZ * ndcY / proj[5];
    out[2] = cameraZ;
}

}   // namespace

TEST (MatrixMath, LeftHandedConversionLeavesTheViewProjectionAlone)
{
    // ⚠️ THE POINT OF THE WHOLE CONVERSION. If this ever fails, the depth
    // buffer the post-processes read no longer matches the matrices they are
    // told about, and the conversion has become the bug it was written to fix.
    const float eye[3] = {7.0f, -9.0f, 5.0f};
    const float at[3] = {1.0f, 2.0f, 0.5f};
    float view[16];
    float proj[16];
    LookAtRH (view, eye, at, kUpZ);
    PerspectiveRH (proj, 45.0f, 16.0f / 9.0f, 0.05f, 20000.0f);

    float viewProj[16];
    Multiply (viewProj, view, proj);

    float viewLh[16];
    float projLh[16];
    ToLeftHandedView (viewLh, view);
    ToLeftHandedProjection (projLh, proj);

    float viewProjLh[16];
    Multiply (viewProjLh, viewLh, projLh);

    for (int k = 0; k < 16; ++k)
        EXPECT_NEAR (viewProjLh[k], viewProj[k], kEps) << "element " << k;
}

TEST (MatrixMath, LeftHandedPairReconstructsTheViewSpacePositionDiligentFXExpects)
{
    const float eye[3] = {7.0f, -9.0f, 5.0f};
    const float at[3] = {1.0f, 2.0f, 0.5f};
    float view[16];
    float proj[16];
    LookAtRH (view, eye, at, kUpZ);
    PerspectiveRH (proj, 45.0f, 16.0f / 9.0f, 0.05f, 20000.0f);

    float viewLh[16];
    float projLh[16];
    ToLeftHandedView (viewLh, view);
    ToLeftHandedProjection (projLh, proj);

    // A point in front of the camera, rasterised through the pair the renderer
    // actually uses -- which, by the test above, is the converted pair too.
    const float world[4] = {3.1f, -0.4f, 2.2f, 1.0f};
    float viewProj[16];
    Multiply (viewProj, view, proj);
    float clip[4];
    TransformPoint (clip, world, viewProj);
    ASSERT_GT (clip[3], 0.0f) << "the sample point must be in front of the camera";

    const float uv[2] = {(clip[0] / clip[3]) * 0.5f + 0.5f, -(clip[1] / clip[3]) * 0.5f + 0.5f};
    const float depth = clip[2] / clip[3];

    float recovered[3];
    ScreenXYDepthToViewSpace (recovered, uv, depth, projLh);

    float expected[4];
    TransformPoint (expected, world, viewLh);
    for (int k = 0; k < 3; ++k)
        EXPECT_NEAR (recovered[k], expected[k], 1e-3f) << "component " << k;
    // ⚠️ POSITIVE, and that is the assertion the whole handedness question comes
    // down to. Under the unconverted pair this is negative, and every consumer
    // that divides by it -- the SSR thickness rejection above all -- inverts.
    EXPECT_GT (recovered[2], 0.0f);
}

TEST (MatrixMath, TheUnconvertedProjectionMirrorsTheReconstruction)
{
    // The bug, pinned as a fact rather than a story: feeding the right-handed
    // projection to DiligentFX's own reconstruction negates x and y and leaves
    // z negative. Recorded so that "it looked warped" has a number behind it.
    float proj[16];
    PerspectiveRH (proj, 45.0f, 16.0f / 9.0f, 0.05f, 20000.0f);

    const float viewPos[4] = {1.3f, -0.7f, -8.0f, 1.0f};
    float clip[4];
    TransformPoint (clip, viewPos, proj);
    const float uv[2] = {(clip[0] / clip[3]) * 0.5f + 0.5f, -(clip[1] / clip[3]) * 0.5f + 0.5f};

    float recovered[3];
    ScreenXYDepthToViewSpace (recovered, uv, clip[2] / clip[3], proj);

    EXPECT_NEAR (recovered[0], -viewPos[0], 1e-3f);
    EXPECT_NEAR (recovered[1], -viewPos[1], 1e-3f);
    EXPECT_NEAR (recovered[2], viewPos[2], 1e-3f);
}

// ---- TAA's sub-pixel jitter -------------------------------------------------
//
// ⚠️ THE SAME HANDEDNESS TRAP, ONE LEVEL UP, AND EVEN LESS VISIBLE THAN THE
// SSR ONE. Diligent's TemporalAntiAliasing::GetJitteredProjMatrix says
// `m20 += Jitter.x`, which is right for its own left-handed projection and
// backwards for PerspectiveRH. Transcribing it shifted the pixel by -jitter
// while CameraAttribs::f2Jitter reported +jitter, so the accumulation resolved
// against a history offset the wrong way. That does not render as a broken
// projection -- it renders as TAA not working, which is indistinguishable from
// TAA being badly tuned. Hence: measured here, in NDC, as a number.

namespace {

// The NDC x/y a view-space point lands on under `proj`.
void ProjectToNdc (float out[2], const float viewPos[3], const float proj[16])
{
    const float p[4] = {viewPos[0], viewPos[1], viewPos[2], 1.0f};
    float clip[4];
    TransformPoint (clip, p, proj);
    out[0] = clip[0] / clip[3];
    out[1] = clip[1] / clip[3];
}

}   // namespace

TEST (MatrixMath, JitterShiftsThePerspectivePixelByTheOffsetItWasGiven)
{
    float proj[16];
    PerspectiveRH (proj, 45.0f, 16.0f / 9.0f, 0.05f, 20000.0f);

    const float jitterX = 0.0031f;
    const float jitterY = -0.0017f;
    float jittered[16];
    JitterProjection (jittered, proj, jitterX, jitterY);

    // ⚠️ TWO DEPTHS, because under a perspective projection the offset lives in
    // row 2 and is therefore scaled by z on the way through. The whole reason it
    // goes there rather than in row 3 is that the NDC shift must come out the
    // SAME at every depth; one sample could not tell that apart from a shear.
    const float near[3] = {1.3f, -0.7f, -8.0f};
    const float far[3] = {-40.0f, 22.0f, -350.0f};
    for (const float* viewPos : {near, far}) {
        float base[2];
        float shifted[2];
        ProjectToNdc (base, viewPos, proj);
        ProjectToNdc (shifted, viewPos, jittered);
        EXPECT_NEAR (shifted[0] - base[0], jitterX, kEps) << "x at z = " << viewPos[2];
        EXPECT_NEAR (shifted[1] - base[1], jitterY, kEps) << "y at z = " << viewPos[2];
    }
}

TEST (MatrixMath, JitterShiftsTheParallelPixelByTheOffsetItWasGiven)
{
    // The affine branch. It was already correct before the handedness fix --
    // the offset is in row 3, which the left-handed conversion does not touch --
    // and this pins that it stayed correct.
    float proj[16];
    OrthographicRH (proj, -10.0f, 10.0f, -6.0f, 6.0f, 0.05f, 400.0f);
    ASSERT_NE (proj[15], 0.0f) << "an orthographic projection is affine";

    const float jitterX = 0.0031f;
    const float jitterY = -0.0017f;
    float jittered[16];
    JitterProjection (jittered, proj, jitterX, jitterY);

    const float viewPos[3] = {1.3f, -0.7f, -8.0f};
    float base[2];
    float shifted[2];
    ProjectToNdc (base, viewPos, proj);
    ProjectToNdc (shifted, viewPos, jittered);
    EXPECT_NEAR (shifted[0] - base[0], jitterX, kEps);
    EXPECT_NEAR (shifted[1] - base[1], jitterY, kEps);
}

TEST (MatrixMath, JitterLeavesTheDepthMappingAlone)
{
    // ⚠️ A JITTER THAT MOVED DEPTH WOULD DESYNC SSR FROM THE BUFFER IT READS.
    // The jittered projection is what renders the G-buffer AND what reaches
    // CameraAttribs::mProj, so a row-2 edit that touched m22 or m23 would put
    // the depth values and the matrix that decodes them out of step.
    float proj[16];
    PerspectiveRH (proj, 45.0f, 16.0f / 9.0f, 0.05f, 20000.0f);
    float jittered[16];
    JitterProjection (jittered, proj, 0.0031f, -0.0017f);

    EXPECT_FLOAT_EQ (jittered[10], proj[10]);
    EXPECT_FLOAT_EQ (jittered[11], proj[11]);
    EXPECT_FLOAT_EQ (jittered[14], proj[14]);

    const float viewPos[4] = {1.3f, -0.7f, -8.0f, 1.0f};
    float clip[4];
    float clipJ[4];
    TransformPoint (clip, viewPos, proj);
    TransformPoint (clipJ, viewPos, jittered);
    EXPECT_NEAR (clipJ[2] / clipJ[3], clip[2] / clip[3], kEps);
}

TEST (MatrixMath, ZeroJitterIsTheProjectionItself)
{
    // What every non-TAA frame passes. A no-op here has to be exactly a no-op,
    // or SSR and AO see a different matrix depending on whether TAA is enabled.
    float proj[16];
    PerspectiveRH (proj, 45.0f, 16.0f / 9.0f, 0.05f, 20000.0f);
    float jittered[16];
    JitterProjection (jittered, proj, 0.0f, 0.0f);
    for (int k = 0; k < 16; ++k)
        EXPECT_FLOAT_EQ (jittered[k], proj[k]) << "element " << k;
}
