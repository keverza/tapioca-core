#ifndef EVP_ARCHVIZ_VIEWPRESETS_HPP
#define EVP_ARCHVIZ_VIEWPRESETS_HPP

// ArchViz/ViewPresets -- the standalone viewer's named axonometric views (issue #88 M3, PLAT-RE137).
//
// ⚠️ AXONOMETRIC IS NOT A THIRD PROJECTION. It is the parallel projection seen from a named
// direction, so a preset sets three things together: the projection (parallel), the direction, and
// the extent that frames the model -- a parallel view has no "fit" that is also a viewpoint, and a
// direction alone leaves the half-height at whatever it held.
//
// ⚠️ THE DIRECTIONS FOLLOW PROJECT NORTH, NOT WORLD +Y. `northDegrees` is counter-clockwise from
// +X, 90 (+Y) when the project supplies none (DiligentSceneStats::northDegrees). With n that angle,
// the eye's direction from the target for each side of the model is
//
//     back  B = ( cos n,  sin n, 0)      front F = -B
//     right R = ( sin n, -cos n, 0)      left  L = -R
//     top   U = ( 0, 0, 1)
//
// and every named view is a sum of those:
//
//     Isometric SE   R + F + U    normalised: all three axes foreshortened alike (0.8165),
//     Isometric SW   L + F + U    the eye 35.264 degrees above the horizon, 45 off each side
//     Isometric NE   R + B + U
//     Isometric NW   L + B + U
//     Dimetric       R/3 + (sqrt 7 / 3) F + U/3
//                    width : height : depth drawn 1 : 1 : 1/2 -- the engineering dimetric
//                    (ISO 5456-3). Eye 19.471 degrees up, 20.705 degrees right of the front.
//     Trimetric      sqrt(3/103) R + sqrt(78/103) F + sqrt(22/103) U
//                    width : height : depth drawn 1 : 0.9 : 0.5, three different scales. Eye
//                    27.527 degrees up, 11.096 degrees right of the front.
//     Top            straight down, project north up the screen: a pose, not an orbit direction
//                    (Camera::SetTopDown -- the orbit's pitch clamp stays where it is).
//
// An axis drawn along the unit direction e is foreshortened by sqrt (1 - (e . d)^2) for the eye
// direction d; the dimetric and trimetric vectors are the solutions of those ratios, and
// test_viewpresets.cpp measures the ratios back off the frame's own view-projection.
//
// ⚠️ PLAN MODE (ViewerPlanMode.hpp). Held to the plan, a preset does nothing: the plan stays the
// held top view. With the orbit freed a preset may turn away from the plan and never clears the
// plan's home; "Top" there is the plan's own home (Camera::ReturnToPlan: its rotation, its cut),
// not a world top view.
//
// Session only: nothing here is written anywhere. Pure -- Camera and <cmath> -- so it is tested
// offline. RENDER THREAD, like the camera.

#include <cstdint>
#include "SunStudy/AnalysisRoi.hpp"

namespace geomsrv {
namespace archviz {

class Camera;

enum class ViewPreset : uint8_t {
    None = 0,
    IsometricNE = 1,
    IsometricNW = 2,
    IsometricSE = 3,
    IsometricSW = 4,
    Dimetric = 5,
    Trimetric = 6,
    Top = 7,
    Count = 8
};

const char* ViewPresetName (ViewPreset preset);

// The eye's unit direction from the target. False, and `out` untouched, for None and for Top,
// which is a pose with an explicit up vector rather than a direction the orbit can hold.
bool ViewPresetDirection (ViewPreset preset, float northDegrees, float out[3]);

// The top-down rotation (Camera::SetTopDown) that puts project north up the screen.
float TopDownRotationForNorth (float northDegrees);
struct ScreenshotRect;
bool FitAnalysisCaptureRect (Camera& camera, const tapioca::sunstudy::AnalysisRoi& roi, float north, float aspect,
                             float bottom, float top, ScreenshotRect rect);

// Build a detached export camera. Scene depth is shared by the panels but
// cannot enlarge their plan extent. On failure output is untouched.
bool FitAnalysisPlanCamera (Camera& output, const tapioca::sunstudy::AnalysisRoi& roi, float northDegrees, float aspect,
                            float sceneBottom, float sceneTop);

enum class ViewPresetOutcome : uint8_t {
    None = 0,          // nothing was asked for
    HeldToPlan = 1,    // plan mode with the orbit held: nothing changed
    Framed = 2,        // parallel, the named direction, the box framed
    DirectionOnly = 3, // parallel and the named direction; no box is known, target and zoom kept
    PlanHome = 4       // "Top" in plan mode: back on the plan's own rotation and cut
};

// What the HUD says under the control after a preset; nullptr when there is nothing to say.
const char* ViewPresetNote (ViewPresetOutcome outcome);

// Apply one preset to the camera, at once.
//
// `frameMin`/`frameMax` is the box to frame -- the selection's when there is one, else the scene's
// -- and `sceneMin`/`sceneMax` the scene's, which stays the box "zoom to fit" and the parallel
// depth range use (Camera::SetBounds, CoverBoundsInDepth). Either pair may be null: no box.
// `aspect` is the surface's width over its height; a surface taller than wide is framed by its
// width.
// #124 Focus in view: frame `lo`..`hi` as the view is turned now -- no projection switch and no
// turn, so a plan stays a plan with its eye on the cut (Camera::FrameBounds). The scene's box, when
// given, stays the one "zoom to fit" and the parallel depth range use. False for an empty or
// non-finite box.
bool FrameFocusBox (Camera& camera, const float lo[3], const float hi[3], const float* sceneMin, const float* sceneMax,
                    float aspect);

ViewPresetOutcome ApplyViewPreset (Camera& camera, ViewPreset preset, float northDegrees, bool planMode, bool planOrbit,
                                   const float* frameMin, const float* frameMax, const float* sceneMin,
                                   const float* sceneMax, float aspect);

} // namespace archviz
} // namespace geomsrv

#endif
