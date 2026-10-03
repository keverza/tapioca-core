#ifndef EVP_ARCHVIZ_VIEWERPLANMODE_HPP
#define EVP_ARCHVIZ_VIEWERPLANMODE_HPP

// ArchViz/ViewerPlanMode -- the separate viewer opened in the floor plan's place
// (ArchVizPanel::ReadPlanViewerCamera): its camera started on the storey's cut, its navigation
// held to the plan unless the user frees it, the top view brought back, the HUD's projection
// toggle followed. What the frame loop (DiligentViewport.cpp, at its frozen size) calls in
// three places, so that file gains the calls and nothing else.
//
// ⚠️ THE PLAN VIEW STAYS A PLAN UNLESS THE USER SAYS OTHERWISE (the user, 2026-10-03: the plan
// view was empty; it needs a way back to the top view once rotated, or the orbit locked out).
// Opened from the plan, the viewer is parallel and straight down with its orbit held -- the
// Shift-drag that orbits elsewhere pans -- and its Settings say "plan": the top view again, and
// the orbit freed for the user who wants to turn it.
//
// RENDER THREAD, like the frame loop. The CameraStart was read on the main thread.

#include "ArchViz/DiligentViewport.hpp" // CameraStart
#include "ArchViz/InputRingBuffer.hpp"  // InputSnapshot

#include <cstdint>

namespace geomsrv {
namespace archviz {

class Camera;
struct HudState;

// ⚠️ THE EYE A LITTLE OVER THE STOREY'S CUT, NOT ON IT: the near plane (Camera::NearClip, 5 cm)
// cuts the model just over the plan's cut, and an outline drawn at the cut is in front of it
// rather than behind the near plane.
constexpr float kPlanEyeAboveCutMetres = 0.1f;

// The camera from Archicad (ApplyArchicadCamera); from a plan with its cut, also the eye over
// the cut, the plan as the camera's home (Camera::SetPlanHome) and the HUD in plan mode --
// parallel, its orbit held. False when there was no camera to copy, as ApplyArchicadCamera.
bool StartViewerCamera (Camera& camera, HudState& hud, const CameraStart& start, uint32_t width, uint32_t height,
                        float* outDistance);

// One frame's navigation: plan mode's controls first -- the orbit held unless freed, the top
// view when asked for (`HudState::planTopView`, cleared here) -- then the input. True when the
// camera moved.
bool NavigateViewer (Camera& camera, HudState& hud, const InputSnapshot& input, uint32_t width, uint32_t height);

// The HUD's projection toggle, on the frame it changed (Camera::SwitchProjection): true then,
// and the frame loop resets its temporal history. `last` is the frame loop's.
bool FollowProjectionToggle (Camera& camera, const HudState& hud, bool& last);

} // namespace archviz
} // namespace geomsrv

#endif
