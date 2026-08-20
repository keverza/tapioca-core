#ifndef EVP_ARCHVIZ_CAMERA_HPP
#define EVP_ARCHVIZ_CAMERA_HPP

// The viewer's orbit camera.
//
// ⚠️ THE NAVIGATION IS ARCHICAD'S, NOT A VIEWER'S OWN. Stated as a requirement
// (plan §8.4) and it replaces the earlier left-drag scheme entirely:
//
//   wheel scroll                zoom, TOWARD THE CURSOR
//   wheel button drag           pan
//   Shift + wheel button drag   orbit
//   double-click wheel button   zoom to fit
//   LEFT BUTTON                 NEVER NAVIGATES — it is selection, and becomes
//                               picking in Phase 8
//
// An architect who has to learn a second set of mouse conventions to look at
// their own model will not use the viewer, so this is not a preference.
//
// ⚠️ Z-UP THROUGHOUT, and that is a decision, not an oversight. Archicad is
// Z-up and metres; bgfx's example maths and shaders assume Y-up. The cheapest
// CORRECT answer is to stay in Z-up and hand bx::mtxLookAt an up vector of
// {0,0,1}, converting nothing — then Archicad's own camera, its sun vector and
// its storey Z levels all drop straight in. The conversion that looks
// equivalent, (x,y,z)->(x,z,y), is a MIRROR (determinant -1) and has shipped in
// this repo once (plan §6.4, AddOn/render-paths-HANDOFF.md).
//
// ⚠️ RIGHT-HANDED MATRICES, EXPLICITLY, AND bx DEFAULTS TO LEFT. Archicad's
// world is right-handed (Z-up, X east, Y north). bx::mtxLookAt and bx::mtxProj
// both take a Handedness argument that DEFAULTS TO Handedness::Left, and a
// left-handed view+projection pair applied to a right-handed world renders the
// whole model MIRRORED — determinant -1, the same class of bug as the
// (x,y,z)->(x,z,y) "axis swap" in plan §6.4 and AddOn/render-paths-HANDOFF.md.
//
// It shipped here once and hid well, because a mirror is only obvious if you
// know what you are looking at. Measured in Archicad 2026-08-06, its two
// symptoms were:
//   - pan, orbit and zoom-to-cursor all inverted HORIZONTALLY and correct
//     vertically (bx's LH `right` is the negation of the RH one; `up` is the
//     same either way — so the asymmetry is the fingerprint);
//   - backface culling apparently needing CULL_CCW, because the mirror had
//     flipped every triangle's screen-space winding.
// Two wrongs that partly cancelled. A debug cube survives being mirrored; a
// building does not — it comes out with north and south swapped.
//
// ⚠️ NO bx TYPES IN THIS HEADER. Camera.cpp is compiled by the `archviz_render`
// target because it needs bx's matrix maths, and bx's headers cannot survive the
// .apx's /Zc:wchar_t-. Matrices cross as plain float[16] so the .apx side can
// still hold a Camera. Same rule as ImGuiBgfx.hpp.
//
// RENDER THREAD ONLY, like everything it feeds.

#include "ArchViz/InputRingBuffer.hpp"

#include <cstdint>

namespace geomsrv {
namespace archviz {

// What a held navigation button is currently doing. Exposed so the HUD can say
// it: "the drag did nothing" and "the drag did the other thing" are opposite
// faults and are otherwise one picture.
enum class NavMode : uint8_t { None = 0, Pan = 1, Orbit = 2 };

class Camera final {
  public:
    // Spherical about a target, which is what an architect expects: the model
    // stays put and the eye moves around it. A free-fly camera is a different
    // interaction and would be a different class.
    void SetTarget (float x, float y, float z);
    void SetDistance (float metres);
    // Point the camera without going through the input path. Yaw is about world
    // Z from +X; pitch is off the XY plane and is clamped just short of the pole
    // for the same reason the drag is — at the pole the up vector and the line
    // of sight are parallel and the view snaps to a random roll.
    void SetOrbit (float yawRadians, float pitchRadians);
    // ---- 2D drawing windows: a parallel projection, straight down -----------
    // ⚠️ THIS EXISTS FOR THE PLAN OVERLAY AND NOTHING ELSE (see PlanCameraMath).
    // A floor plan is not a camera at all -- it is an orthographic top view at a
    // known scale -- so slaving the overlay to one needs both halves: a
    // projection with no perspective divide, and a pose looking exactly along
    // -Z, which the ordinary orbit deliberately cannot reach (kPitchLimit stops
    // just short of the pole, because at the pole an ORBIT loses its roll).
    //
    // `halfHeightMetres` is half the window's height in model metres. Setting it
    // to zero, or `on` to false, restores the perspective projection.
    void SetOrthographic (bool on, float halfHeightMetres);
    bool IsOrthographic () const
    {
        return orthographic_;
    }
    // The two numbers that, with GetTarget, are the whole plan pose. Exposed for
    // the desync measurement (PLAT-RE84): the render thread logs what it just
    // PRESENTED, and an offline pass compares that against what Archicad had at
    // the same instant. Without these the comparison would have to re-derive the
    // pose from the view matrix, which is the same number laundered through two
    // more conversions.
    float OrthoHalfHeightMetres () const
    {
        return orthoHalfHeight_;
    }
    float TopDownRotationRadians () const
    {
        return topDownRotation_;
    }
    // Look straight down, with `rotationRadians` the CCW angle of the SCREEN's
    // +X axis in model space -- 0 puts model north (+Y) up, which is an
    // unrotated plan. Any later orbit input leaves this pose in the ordinary way.
    void SetTopDown (float rotationRadians);

    // ⚠️ VERTICAL, in degrees. Archicad's own `viewCone` is HORIZONTAL, so a
    // caller copying Archicad's camera must convert with the aspect ratio
    // first; passing the horizontal number straight in gives a view that is too
    // wide by exactly the aspect ratio and looks like a wrong camera position.
    void SetFovDegreesVertical (float degrees);
    // Frame an axis-aligned box: point at its centre and back off far enough to
    // see all of it at the current field of view. This is what Phase 6 calls
    // once the first snapshot lands. The box is REMEMBERED, so "zoom to fit"
    // (the wheel double-click, and the HUD button) needs no argument and cannot
    // drift out of step with what is on screen.
    void FrameBounds (const float minXyz[3], const float maxXyz[3]);

    // Remember a box WITHOUT moving the camera.
    //
    // ⚠️ WITHOUT THIS, "ZOOM TO FIT" DOES NOTHING ON THE PATH THAT MATTERS MOST.
    // FrameLastBounds can only re-frame a box FrameBounds was once given -- and
    // FrameBounds is deliberately NOT called when Archicad supplied a starting
    // camera, because framing would throw away the view the user asked to start
    // from. So on every ordinary open the camera had no bounds at all, and the
    // wheel double-click was silently a no-op: the one gesture that recovers a
    // lost view, unavailable precisely because the view came from Archicad.
    void SetBounds (const float minXyz[3], const float maxXyz[3]);
    bool HasBounds () const
    {
        return haveBounds_;
    }
    // Re-frame whatever FrameBounds was last given. Does nothing if it never was.
    void FrameLastBounds ();

    // Fold one frame's input in. Returns true if the camera actually moved, so
    // the caller can tell a still frame from a moving one.
    //
    // The viewport size is needed, not optional: zoom-to-cursor and a pan that
    // tracks the cursor exactly are both defined in terms of the projected size
    // of the focal plane, and that is pixels-per-metre.
    //
    // ⚠️ `imguiWantsMouse` GATES THE LATCH, NOT THE DRAG. It stops a NEW pan or
    // orbit starting on top of a HUD window; it must NOT stop one already
    // running, or a pan that begins in the viewport dies the moment the cursor
    // crosses the panel (plan §8.4). Only ImGui can answer whether the cursor is
    // over one of its windows, so the caller passes the answer in.
    bool ApplyInput (const InputSnapshot& input, bool imguiWantsMouse, uint32_t viewportWidth, uint32_t viewportHeight);

    // Column-major, ready for bgfx::setViewTransform.
    void GetViewMatrix (float out[16]) const;
    void GetProjMatrix (float out[16], float aspect) const;

    // Where the eye is, world space. The shader needs it for anything
    // view-dependent, and the ImGui readout needs it to be diagnosable.
    void GetEyePosition (float out[3]) const;
    // What it is looking AT. Archicad describes its own perspective camera as
    // pos + target, so the navigation log compares like with like (NavLog.hpp).
    void GetTarget (float out[3]) const;

    float Distance () const
    {
        return distance_;
    }
    // One source for projection, depth diagnostics and DiligentFX CameraAttribs.
    static constexpr float NearClip ()
    {
        return 0.05f;
    }
    float FarClip () const
    {
        return orthographic_ ? distance_ * 2.0f : 20000.0f;
    }
    bool IsPerspective () const
    {
        return !orthographic_;
    }
    // ⚠️ VERTICAL. Archicad's own `viewCone` is HORIZONTAL — the navigation log
    // puts both in one column, so which one this is has to be readable from
    // here rather than assumed at the call site.
    float FovDegreesVertical () const
    {
        return fovDegrees_;
    }

    // The world-space ray through a viewport pixel: `origin` on the near side,
    // `direction` normalised. Handles both projections -- under a parallel camera
    // the direction is constant and the CURSOR MOVES THE ORIGIN, which is the
    // same fact PLAT-RE62 turned on for picking.
    //
    // ⚠️ THE BASIS COMES FROM MatrixMath::CameraBasis, shared with the pick's
    // Aim. Two hand-rolled copies of one camera's frame is precisely what Aim's
    // "not by inverting the view-projection" comment warns about, one level up.
    void CursorRay (int32_t px, int32_t py, uint32_t width, uint32_t height, float origin[3], float direction[3]) const;
    float YawDegrees () const;
    float PitchDegrees () const;
    NavMode CurrentNav () const
    {
        return nav_;
    }

  private:
    // The camera's right and up axes in WORLD space, Z-up. Pan, zoom-to-cursor
    // and any future unprojection all need them, and deriving them twice is how
    // one of them ends up with a sign the other does not have.
    void Basis (float right[3], float up[3]) const;
    // Half-height of the focal plane (the plane through the target, normal to
    // the view direction) in metres. Everything screen-to-world scales by it.
    float FocalHalfHeight () const;

    float targetX_ = 0.0f, targetY_ = 0.0f, targetZ_ = 0.0f;
    float distance_ = 12.0f; // metres
    // Radians. Yaw about world Z (Archicad's up), pitch off the XY plane.
    float yaw_ = -0.9f;
    float pitch_ = 0.6f;

    // What FrameBounds was last handed, for "zoom to fit".
    float boundsMin_[3] = { 0.0f, 0.0f, 0.0f };
    float boundsMax_[3] = { 0.0f, 0.0f, 0.0f };
    bool haveBounds_ = false;

    // ---- navigation state -------------------------------------------------
    NavMode nav_ = NavMode::None;
    bool navWasDown_ = false; // previous frame's polled wheel-button state
    // Steady-clock milliseconds of the last wheel-button PRESS, for the
    // double-click. Kept as a plain integer so no <chrono> reaches this header.
    uint64_t lastNavDownMs_ = 0;

    // The previous cursor position, so a MOVE becomes a DELTA. The snapshot
    // carries absolute positions on purpose (a delta queue is lossy in the wrong
    // direction — plan §1.1), so the differencing happens here.
    int32_t lastX_ = 0, lastY_ = 0;
    bool haveLast_ = false;

    // ---- the plan-overlay pose (SetOrthographic / SetTopDown) ---------------
    bool orthographic_ = false;
    float orthoHalfHeight_ = 0.0f; // model metres, half the surface's height
    // ⚠️ A SEPARATE FLAG FROM `orthographic_`, because they answer different
    // questions: one is the PROJECTION and one is the POSE. Only the pose needs
    // an explicit up vector (at the pole the orbit's derived basis is
    // degenerate), and only the projection changes GetProjMatrix.
    bool topDown_ = false;
    float topDownRotation_ = 0.0f; // radians, CCW, of the SCREEN's +X in model space

    // Field of view, degrees, VERTICAL. ⚠️ Archicad's own `viewCone` is
    // HORIZONTAL and in degrees (confirmed, see the projection-overlay work);
    // whichever way this camera is later slaved to Archicad's, the conversion is
    // not optional.
    float fovDegrees_ = 45.0f;
};

} // namespace archviz
} // namespace geomsrv

#endif
