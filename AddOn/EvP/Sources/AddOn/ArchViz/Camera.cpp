#include "ArchViz/Camera.hpp"

#include "ArchViz/MatrixMath.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace geomsrv {
namespace archviz {

namespace {

// ⚠️ THIS FILE IS NO LONGER bx's. It used bx::mtxLookAt/bx::mtxProj and bx's
// scalar helpers, which is why it lived in the `archviz_render` static library
// and could not see a DevKit type. The two matrices moved to
// ArchViz/MatrixMath, which reproduces bx's right-handed non-homogeneous forms
// element for element and HAS OFFLINE TESTS (tests/cpp/test_matrixmath.cpp) —
// the scalar helpers were std:: all along under another name. Same numbers,
// same picture, no seam. Do not reintroduce a bx include here.

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kRadToDeg = 180.0f / kPi;

// Radians per pixel of drag. Tuned so a drag across a 1000 px viewport is a
// little over half a turn — enough to get round a building without letting go,
// slow enough to aim.
constexpr float kOrbitPerPixel = 0.006f;
// Fraction of the orbit distance per wheel notch. MULTIPLICATIVE, not additive:
// a fixed step is unusably coarse when you are close and unusably slow when you
// are far, and a building is looked at from both.
constexpr float kDollyPerNotch = 0.12f;

constexpr float kMinDistance = 0.05f;
constexpr float kMaxDistance = 100000.0f;
// Just short of straight up/down. AT the pole the up vector and the view
// direction are parallel and mtxLookAt produces a degenerate matrix — the view
// snaps to a random roll, which reads as the model spinning.
constexpr float kPitchLimit = 1.5533f; // ~89 degrees

// Windows' own default double-click time is 500 ms; 400 is inside it and short
// enough that two deliberate pans in a row are never mistaken for a fit.
constexpr uint64_t kDoubleClickMs = 400;

uint64_t NowMs ()
{
    using namespace std::chrono;
    return uint64_t (duration_cast<milliseconds> (steady_clock::now ().time_since_epoch ()).count ());
}

} // namespace

void Camera::SetTarget (float x, float y, float z)
{
    targetX_ = x;
    targetY_ = y;
    targetZ_ = z;
}

void Camera::SetDistance (float metres)
{
    distance_ = std::clamp (metres, kMinDistance, kMaxDistance);
}

void Camera::SetOrbit (float yawRadians, float pitchRadians)
{
    yaw_ = yawRadians;
    pitch_ = std::clamp (pitchRadians, -kPitchLimit, kPitchLimit);
    // Any ordinary orbit leaves the plan pose: the two are different ways of
    // aiming the same camera, and the last one asked for wins.
    topDown_ = false;
}

void Camera::SetOrthographic (bool on, float halfHeightMetres)
{
    orthographic_ = on && halfHeightMetres > 1e-4f;
    orthoHalfHeight_ = orthographic_ ? halfHeightMetres : 0.0f;
}

void Camera::SetTopDown (float rotationRadians)
{
    topDown_ = true;
    topDownRotation_ = rotationRadians;
    // ⚠️ SET DIRECTLY, PAST kPitchLimit, AND THAT IS THE POINT OF THIS FUNCTION.
    // A plan is straight down and nothing else; 89 degrees would tilt the model
    // by a metre of parallax across a twenty-metre building, which reads as the
    // overlay being slightly out of register rather than as a deliberate clamp.
    // The clamp still applies to the ORBIT, where it belongs -- there it stops a
    // random roll, and here the roll is supplied explicitly by
    // `topDownRotation_` instead of being derived.
    pitch_ = kPi * 0.5f;
    // Keeps GetEyePosition (which is shared with the orbit path) directly above
    // the target; the screen's roll comes from the up vector in GetViewMatrix.
    yaw_ = 0.0f;
}

void Camera::SetFovDegreesVertical (float degrees)
{
    fovDegrees_ = std::clamp (degrees, 1.0f, 170.0f);
}

void Camera::FrameBounds (const float minXyz[3], const float maxXyz[3])
{
    for (int i = 0; i < 3; ++i) {
        boundsMin_[i] = minXyz[i];
        boundsMax_[i] = maxXyz[i];
    }
    haveBounds_ = true;

    SetTarget ((minXyz[0] + maxXyz[0]) * 0.5f, (minXyz[1] + maxXyz[1]) * 0.5f, (minXyz[2] + maxXyz[2]) * 0.5f);

    const float dx = maxXyz[0] - minXyz[0];
    const float dy = maxXyz[1] - minXyz[1];
    const float dz = maxXyz[2] - minXyz[2];
    const float radius = 0.5f * std::sqrt (dx * dx + dy * dy + dz * dz);

    // Back off by the bounding SPHERE, not the box: it is orientation-
    // independent, so the model does not fall out of frame when the camera
    // orbits to a corner. 1.4 is slack for the fact that a sphere around a box
    // is generous in one direction and tight in another.
    const float halfFov = fovDegrees_ * kDegToRad * 0.5f;
    SetDistance (radius > 0.0f ? (radius / std::sin (halfFov)) * 1.4f : 12.0f);

    // ⚠️ THE PARALLEL CAMERA IS FRAMED BY ITS HALF-HEIGHT, NOT BY ITS DISTANCE,
    // AND FORGETTING THAT MADE THE MODEL VANISH IN AXONOMETRIC. Backing a
    // parallel camera off changes nothing about what it sees -- the extent does
    // -- so framing that only moved the eye left `orthoHalfHeight_` at whatever
    // it happened to hold, which on a project far from the origin is a window
    // somewhere else entirely. "Zoom to fit" then appeared to do nothing.
    if (orthographic_)
        orthoHalfHeight_ = radius > 0.0f ? radius * 1.15f : 12.0f;
}

void Camera::SetBounds (const float minXyz[3], const float maxXyz[3])
{
    for (int i = 0; i < 3; ++i) {
        boundsMin_[i] = minXyz[i];
        boundsMax_[i] = maxXyz[i];
    }
    haveBounds_ = true;
}

void Camera::FrameLastBounds ()
{
    if (haveBounds_)
        FrameBounds (boundsMin_, boundsMax_);
}

void Camera::Basis (float right[3], float up[3]) const
{
    const float cp = std::cos (pitch_);
    const float sp = std::sin (pitch_);
    const float cy = std::cos (yaw_);
    const float sy = std::sin (yaw_);

    // Derived from eye = target + d*(cp*cy, cp*sy, sp) with world up {0,0,1}:
    // right = normalize (forward x worldUp), up = right x forward. Both come out
    // unit-length for any |pitch| < 90 degrees, which kPitchLimit guarantees.
    //
    // ⚠️ THIS IS THE RIGHT-HANDED CONVENTION, AND IT ONLY MATCHES THE VIEW
    // MATRIX BECAUSE GetViewMatrix ASKS FOR Handedness::Right. Read bx's own
    // mtxLookAt: it builds `right = cross (worldUp, view)`, which for its
    // DEFAULT Handedness::Left is the exact NEGATION of the line below — so
    // under the default this basis is X-mirrored against the matrix actually
    // being rendered with, and every screen-to-world quantity derived from it
    // (pan, orbit, zoom-to-cursor) comes out backwards HORIZONTALLY while
    // staying correct vertically. That asymmetry is the fingerprint; it was
    // measured exactly that way in Archicad on 2026-08-06. If pan ever inverts
    // on X alone again, look here and at GetViewMatrix, not at these signs.
    right[0] = -sy;
    right[1] = cy;
    right[2] = 0.0f;
    up[0] = -cy * sp;
    up[1] = -sy * sp;
    up[2] = cp;
}

float Camera::FocalHalfHeight () const
{
    // In a parallel projection the "focal plane" is every plane: the half-height
    // does not depend on the distance, it IS the frustum's half-height. Pan and
    // zoom-to-cursor both scale by this, so without the branch a plan view would
    // pan at whatever speed the (unused) field of view implied.
    if (orthographic_)
        return orthoHalfHeight_;
    return distance_ * std::tan (fovDegrees_ * kDegToRad * 0.5f);
}

bool Camera::ApplyInput (const InputSnapshot& input, bool imguiWantsMouse, uint32_t viewportWidth,
                         uint32_t viewportHeight)
{
    bool moved = false;

    const float w = float (viewportWidth > 0 ? viewportWidth : 1);
    const float h = float (viewportHeight > 0 ? viewportHeight : 1);

    // ---- the navigation button: latch on the press EDGE --------------------
    // ⚠️ EDGE-DETECTED FROM A POLLED LEVEL, not from a queued event. The polled
    // state is continuous for the whole drag, which is the entire fix in plan
    // §8.4 — a queued down/up pair can be read at the wrong moment and yields
    // one huge delta.
    if (input.navButton && !navWasDown_) {
        // ⚠️ `inside` IS NOT A NICETY HERE, IT IS THE ONLY THING STOPPING US
        // STEALING ARCHICAD'S OWN NAVIGATION. The wheel button is read from the
        // whole desktop (GetAsyncKeyState is global — see PollHardwareInput), so
        // without this a middle-drag in Archicad's 3D window drove BOTH cameras
        // at once, which is exactly what the 2026-08-06 run reported. Only the
        // PRESS is gated: a drag already latched must keep running wherever the
        // cursor goes, or panning across the HUD dies at its edge.
        const bool ours = input.inside && !imguiWantsMouse;

        const uint64_t nowMs = NowMs ();
        const bool isDoubleClick = lastNavDownMs_ != 0 && (nowMs - lastNavDownMs_) <= kDoubleClickMs;

        if (isDoubleClick && ours) {
            // Archicad's "zoom to fit". The second press must NOT also start a
            // drag, or the fit is immediately panned away from by the few pixels
            // the hand moves on the way to letting go.
            FrameLastBounds ();
            nav_ = NavMode::None;
            lastNavDownMs_ = 0;
            moved = true;
        }
        else {
            lastNavDownMs_ = nowMs;
            // ⚠️ THE ONLY PLACE imguiWantsMouse OR inside IS CONSULTED. Once
            // latched, the drag survives the cursor crossing the HUD — and
            // leaving the viewport entirely.
            if (ours)
                nav_ = input.shift ? NavMode::Orbit : NavMode::Pan;
        }
    }
    else if (!input.navButton && navWasDown_) {
        nav_ = NavMode::None;
    }
    navWasDown_ = input.navButton;

    // Shift is read LIVE, not latched with the button: in Archicad, adding Shift
    // part-way through a pan turns it into an orbit without letting go, and
    // that is a habit worth not breaking.
    if (nav_ != NavMode::None)
        nav_ = input.shift ? NavMode::Orbit : NavMode::Pan;

    // ---- wheel: zoom TOWARD THE CURSOR -------------------------------------
    if (!imguiWantsMouse && input.wheelDelta != 0) {
        // Multiplicative: each notch changes the distance by a FRACTION, so the
        // zoom feels the same at 2 m and at 200 m.
        const float wanted = distance_ * std::pow (1.0f - kDollyPerNotch, float (input.wheelDelta) / 120.0f);
        const float clamped = std::clamp (wanted, kMinDistance, kMaxDistance);
        // The factor ACTUALLY applied, which is not the requested one at the
        // limits. Anchoring with the requested factor would slide the target
        // sideways every notch once the zoom has bottomed out.
        const float applied = clamped / distance_;

        if (input.inside && applied != 1.0f) {
            // The world point under the cursor, on the focal plane. Keeping THAT
            // point fixed on screen while the distance changes is what "zoom
            // toward the cursor" means: screen scale is proportional to
            // distance, so target' = P + applied * (target - P).
            float right[3], up[3];
            Basis (right, up);
            const float halfH = FocalHalfHeight ();
            const float halfW = halfH * (w / h);
            const float sx = (2.0f * float (input.x) / w) - 1.0f;
            const float sy = 1.0f - (2.0f * float (input.y) / h); // screen y is DOWN

            const float px = targetX_ + right[0] * sx * halfW + up[0] * sy * halfH;
            const float py = targetY_ + right[1] * sx * halfW + up[1] * sy * halfH;
            const float pz = targetZ_ + right[2] * sx * halfW + up[2] * sy * halfH;

            targetX_ = px + applied * (targetX_ - px);
            targetY_ = py + applied * (targetY_ - py);
            targetZ_ = pz + applied * (targetZ_ - pz);
        }

        distance_ = clamped;
        moved = true;
    }

    // ---- the drag ----------------------------------------------------------
    const int32_t dx = haveLast_ ? (input.x - lastX_) : 0;
    const int32_t dy = haveLast_ ? (input.y - lastY_) : 0;
    lastX_ = input.x;
    lastY_ = input.y;
    haveLast_ = true;

    if (nav_ == NavMode::None || (dx == 0 && dy == 0))
        return moved;

    if (nav_ == NavMode::Orbit) {
        yaw_ -= float (dx) * kOrbitPerPixel;
        // Dragging DOWN looks down at the model, which is the convention every
        // CAD orbit uses; inverting it here is the single most complained-about
        // thing a viewer can get wrong.
        pitch_ = std::clamp (pitch_ + float (dy) * kOrbitPerPixel, -kPitchLimit, kPitchLimit);
        moved = true;
    }
    else {
        // ⚠️ EXACT, not a tuned constant. One pixel of drag moves the focal
        // plane by exactly one pixel's worth of world, so the model tracks the
        // cursor instead of merely following it — which is what Archicad's pan
        // does and what makes a fudge factor immediately noticeable.
        float right[3], up[3];
        Basis (right, up);
        const float metresPerPixel = 2.0f * FocalHalfHeight () / h;

        targetX_ -= (right[0] * float (dx) - up[0] * float (dy)) * metresPerPixel;
        targetY_ -= (right[1] * float (dx) - up[1] * float (dy)) * metresPerPixel;
        targetZ_ -= (right[2] * float (dx) - up[2] * float (dy)) * metresPerPixel;
        moved = true;
    }

    return moved;
}

void Camera::GetEyePosition (float out[3]) const
{
    const float cp = std::cos (pitch_);
    out[0] = targetX_ + distance_ * cp * std::cos (yaw_);
    out[1] = targetY_ + distance_ * cp * std::sin (yaw_);
    out[2] = targetZ_ + distance_ * std::sin (pitch_);
}

void Camera::GetTarget (float out[3]) const
{
    out[0] = targetX_;
    out[1] = targetY_;
    out[2] = targetZ_;
}

void Camera::CursorRay (int32_t px, int32_t py, uint32_t width, uint32_t height, float origin[3],
                        float direction[3]) const
{
    float eye[3];
    float target[3];
    GetEyePosition (eye);
    GetTarget (target);

    float forward[3];
    float right[3];
    float up[3];
    CameraBasis (eye, target, forward, right, up);

    constexpr float kPi = 3.14159265358979323846f;
    const float aspect = (height > 0) ? float (width) / float (height) : 1.0f;
    // Cursor to NDC. y is flipped because a window's pixels count down from the
    // top and NDC counts up.
    // px + 0.5: a pixel's NDC address is its CENTRE, not its top-left edge. Same
    // correction, same reason, as DiligentPickBuffer::Aim.
    const float ndcX = (width > 0) ? (2.0f * (float (px) + 0.5f) / float (width) - 1.0f) : 0.0f;
    const float ndcY = (height > 0) ? (1.0f - 2.0f * (float (py) + 0.5f) / float (height)) : 0.0f;

    if (orthographic_ && orthoHalfHeight_ > 0.0f) {
        // Parallel: every ray runs along the view direction and the cursor slides
        // the ORIGIN across the image plane.
        const float halfWidth = orthoHalfHeight_ * aspect;
        for (int k = 0; k < 3; ++k) {
            origin[k] = eye[k] + right[k] * (ndcX * halfWidth) + up[k] * (ndcY * orthoHalfHeight_);
            direction[k] = forward[k];
        }
        return;
    }

    const float tanY = std::tan (fovDegrees_ * 0.5f * (kPi / 180.0f));
    float dir[3];
    for (int k = 0; k < 3; ++k)
        dir[k] = forward[k] + right[k] * (ndcX * tanY * aspect) + up[k] * (ndcY * tanY);
    const float length = std::sqrt (dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    const float inv = (length > 1e-6f) ? 1.0f / length : 1.0f;
    for (int k = 0; k < 3; ++k) {
        origin[k] = eye[k];
        direction[k] = dir[k] * inv;
    }
}

void Camera::GetViewMatrix (float out[16]) const
{
    float eye[3];
    GetEyePosition (eye);
    // ⚠️ UP IS {0,0,1}. See the header — this is the whole Z-up decision, in one
    // argument. Do not "fix" it to {0,1,0} to match the bgfx examples.
    // Handedness is no longer an argument that can be got wrong: LookAtRH does
    // right-handed and nothing else, which is why it is named that way.
    const float at[3] = { targetX_, targetY_, targetZ_ };

    // ⚠️ THE TOP-DOWN POSE NEEDS A HORIZONTAL UP VECTOR, and {0,0,1} is exactly
    // the one it cannot use: looking straight down, the world up and the line of
    // sight are parallel, LookAtRH takes its degenerate fallback (+X) and the
    // plan comes out rotated by whatever that fallback happens to be. Supplying
    // the roll explicitly is what makes a ROTATED plan view reproducible --
    // rotation 0 gives up = +Y, i.e. model north up the screen.
    if (topDown_) {
        const float up[3] = { -std::sin (topDownRotation_), std::cos (topDownRotation_), 0.0f };
        LookAtRH (out, eye, at, up);
        return;
    }

    const float up[3] = { 0.0f, 0.0f, 1.0f };
    LookAtRH (out, eye, at, up);
}

void Camera::GetProjMatrix (float out[16], float aspect) const
{
    // ⚠️ THE PARALLEL PROJECTION IS NOT A STYLE, IT IS THE PLAN OVERLAY'S WHOLE
    // REQUIREMENT. A floor plan has no vanishing point: every wall is drawn at
    // its true plan position whatever its height, so a perspective overlay would
    // register only at the storey the camera happens to be aimed at and splay
    // outward from the centre everywhere else -- which reads as a scale error.
    //
    // ⚠️ THE DEPTH RANGE IS TIED TO `distance_`, so the caller sets that big
    // enough to clear the model from above. near is a small positive number
    // because a right-handed projection with near <= 0 is not a projection; far
    // is twice the eye height, which covers everything from `distance_` above
    // the target down to `distance_` below it.
    if (orthographic_) {
        const float halfHeight = orthoHalfHeight_;
        const float halfWidth = halfHeight * (aspect > 0.0f ? aspect : 1.0f);
        OrthographicRH (out, -halfWidth, halfWidth, -halfHeight, halfHeight, NearClip (), FarClip ());
        return;
    }

    // ⚠️ DEPTH 0..1. This used to read `bgfx::getCaps ()->homogeneousDepth`,
    // which on this add-on's D3D11-only build always answered false — and
    // Diligent's D3D11 backend is 0..1 as well, so the CAPS query was a runtime
    // lookup of a compile-time fact and the only thing it bought was a bgfx
    // dependency in the camera. PerspectiveRH offers 0..1 alone. If an OpenGL
    // backend is ever compiled in, this is the line that has to grow a
    // parameter; getting it wrong inverts the depth test over half the scene
    // rather than failing outright.
    // ⚠️ Right-handed, TO MATCH THE VIEW MATRIX. Mixing the two is worse than
    // picking either consistently: the view supplies the basis and the
    // projection supplies the sign of the view-space z it expects, and a
    // mismatch does not mirror the image, it turns the scene inside out
    // through the near plane.
    PerspectiveRH (out, fovDegrees_, aspect, NearClip (), FarClip ());
}

float Camera::YawDegrees () const
{
    return yaw_ * kRadToDeg;
}
float Camera::PitchDegrees () const
{
    return pitch_ * kRadToDeg;
}

} // namespace archviz
} // namespace geomsrv
