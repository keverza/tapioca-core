// ArchViz/ViewerPlanMode -- see the header.

#include "ArchViz/ViewerPlanMode.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/Camera.hpp"
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/DiligentViewportSupport.hpp" // ApplyArchicadCamera
#include "ArchViz/OrbitAnchor.hpp"
#include "ArchViz/StorySliceLayer.hpp"
#include "ArchViz/ViewPresets.hpp"
#include "ArchViz/ViewerEditor.hpp"

#include <cmath>
#include <string>

namespace geomsrv {
namespace archviz {

bool StartViewerCamera (Camera& camera, HudState& hud, const CameraStart& start, uint32_t width, uint32_t height,
                        float* outDistance)
{
    if (!ApplyArchicadCamera (camera, start, width, height, outDistance))
        return false;
    if (!start.orthographic || !start.cut)
        return true;
    // Straight over the plan's middle, the eye just over the storey's cut: what is above it is
    // behind the camera, and the storey and what lies below it show, as on the floor plan.
    float target[3];
    camera.GetTarget (target);
    const float eyeZ = start.cutZ + kPlanEyeAboveCutMetres;
    camera.SetTarget (target[0], target[1], eyeZ - camera.Distance ());
    camera.SetPlanHome (start.planRotationRadians, eyeZ, start.orthoHalfHeightMetres);
    camera.SetOrbitLocked (true);
    hud.planMode = true;
    hud.planOrbit = false;
    // The projection toggle reads what the view is: parallel (the frame loop's edge starts here).
    hud.orthographic = true;
    return true;
}

namespace {

// ⚠️ ONLY WHILE THE NAVIGATION BUTTON IS DOWN: two passes over every element are not a per-frame
// cost for a still view, and only a drag turns about the anchor. A press re-reads it, so a click
// that changed the selection is the anchor of the next orbit.
void FollowOrbitAnchor (Camera& camera, HudState& hud, const DiligentScene& scene, const InputSnapshot& input)
{
    if (!input.navButton)
        return;
    float selMin[3], selMax[3], sceneMin[3], sceneMax[3];
    const bool haveSelection = scene.SelectionBounds (selMin, selMax);
    const bool haveScene = scene.SceneBounds (sceneMin, sceneMax);
    const OrbitAnchor anchor = ChooseOrbitAnchor (haveSelection ? selMin : nullptr, haveSelection ? selMax : nullptr,
                                                  haveScene ? sceneMin : nullptr, haveScene ? sceneMax : nullptr);
    if (anchor.source == OrbitAnchorSource::None)
        camera.ClearOrbitPivot ();
    else
        camera.SetOrbitPivot (anchor.point);
    if (int (anchor.source) == hud.orbitAnchor)
        return;
    hud.orbitAnchor = int (anchor.source);
    ArchVizLog (std::string ("viewer orbit anchor: ") + OrbitAnchorName (anchor.source) + " at " +
                std::to_string (anchor.point[0]) + "," + std::to_string (anchor.point[1]) + "," +
                std::to_string (anchor.point[2]));
}

std::string DescribeView (const Camera& camera)
{
    float target[3];
    camera.GetTarget (target);
    return std::string (camera.IsOrthographic () ? "orthographic" : "perspective") + ", target " +
           std::to_string (target[0]) + "," + std::to_string (target[1]) + "," + std::to_string (target[2]) +
           ", distance " + std::to_string (camera.Distance ()) +
           (camera.IsOrthographic () ? ", half-height " + std::to_string (camera.OrthoHalfHeightMetres ()) : "");
}

// The HUD's named view (`HudState::viewPreset`, a one-shot cleared here whatever comes of it):
// the selection framed when there is one, else the scene.
bool FollowViewPreset (Camera& camera, HudState& hud, const DiligentScene& scene, uint32_t width, uint32_t height)
{
    const ViewPreset preset = ViewPreset (hud.viewPreset);
    hud.viewPreset = 0;
    if (preset == ViewPreset::None || preset >= ViewPreset::Count)
        return false;
    float selMin[3], selMax[3], sceneMin[3], sceneMax[3];
    const bool haveSelection = scene.SelectionBounds (selMin, selMax);
    const bool haveScene = scene.SceneBounds (sceneMin, sceneMax);
    const float* frameMin = haveSelection ? selMin : haveScene ? sceneMin : nullptr;
    const float* frameMax = haveSelection ? selMax : haveScene ? sceneMax : nullptr;
    const float north = scene.Stats ().northDegrees;
    const ViewPresetOutcome outcome = ApplyViewPreset (
        camera, preset, north, hud.planMode, hud.planOrbit, frameMin, frameMax, haveScene ? sceneMin : nullptr,
        haveScene ? sceneMax : nullptr, height > 0 ? float (width) / float (height) : 1.0f);
    hud.viewPresetOutcome = int (outcome);
    if (outcome == ViewPresetOutcome::None || outcome == ViewPresetOutcome::HeldToPlan) {
        ArchVizLog (std::string ("viewer view preset: ") + ViewPresetName (preset) + " not applied (held to the plan)");
        return false;
    }
    // Parallel now: the toggle follows, and its edge finds the camera already so.
    hud.orthographic = true;
    const char* what = outcome == ViewPresetOutcome::PlanHome        ? "the plan's home"
                       : outcome == ViewPresetOutcome::DirectionOnly ? "direction only, no bounds yet"
                       : haveSelection                               ? "framed the selection"
                                                                     : "framed the scene";
    ArchVizLog (std::string ("viewer view preset: ") + ViewPresetName (preset) + " (" + what + "), north " +
                std::to_string (north) + ", " + DescribeView (camera));
    return true;
}

// #124: the editor's Focus in view (vieweredit::RequestFocus, a one-shot box from the main
// thread), framed with this camera as it is turned now. True when it moved the camera.
bool FollowEditorFocus (Camera& camera, const DiligentScene& scene, uint32_t width, uint32_t height)
{
    vieweredit::FocusBox focus;
    if (!vieweredit::TakeFocusRequest (focus))
        return false;
    float sceneMin[3], sceneMax[3];
    const bool haveScene = scene.SceneBounds (sceneMin, sceneMax);
    if (!FrameFocusBox (camera, focus.lo.data (), focus.hi.data (), haveScene ? sceneMin : nullptr,
                        haveScene ? sceneMax : nullptr, height > 0 ? float (width) / float (height) : 1.0f))
        return false;
    ArchVizLog ("viewer editor focus: " + DescribeView (camera));
    return true;
}

} // namespace

bool NavigateViewer (Camera& camera, HudState& hud, const DiligentScene& scene, const InputSnapshot& input,
                     uint32_t width, uint32_t height)
{
    bool moved = false;
    camera.SetNavigationSmoothing (hud.navSmoothing);
    if (hud.planMode) {
        camera.SetOrbitLocked (!hud.planOrbit);
        if (hud.planTopView) {
            hud.planTopView = false;
            camera.ReturnToPlan ();
            // Parallel again: the toggle follows, and its edge finds the camera already so.
            hud.orthographic = true;
            moved = true;
        }
    }
    moved = FollowViewPreset (camera, hud, scene, width, height) || moved;
    moved = FollowEditorFocus (camera, scene, width, height) || moved;
    FollowOrbitAnchor (camera, hud, scene, input);
    moved = camera.ApplyInput (input, hud.wantsMouse || hud.viewpointOwnsMouse, width, height) || moved;
    // A pan or an orbit about the anchor moved a parallel view's target: the model stays inside
    // its depth range. Not the top-down pose -- the plan's eye is on its cut, and a plan slaved to
    // Archicad is not this function's to move.
    if (moved && !camera.IsTopDown ())
        camera.CoverBoundsInDepth ();
    return moved;
}

void AdoptSceneBounds (Camera& camera, const DiligentScene& scene, bool focus)
{
    float boundsMin[3], boundsMax[3];
    if (!scene.SceneBounds (boundsMin, boundsMax))
        return;
    camera.SetBounds (boundsMin, boundsMax);
    const float before = camera.Distance ();
    const float middle[3] = { 0.5f * (boundsMin[0] + boundsMax[0]), 0.5f * (boundsMin[1] + boundsMax[1]),
                              0.5f * (boundsMin[2] + boundsMax[2]) };
    // Logged when it changes the scale, not every batch that lands on the same depth.
    if (focus && camera.FocusAtDepthOf (middle) && std::abs (camera.Distance () - before) > 0.05f * before)
        ArchVizLog ("viewer focus at the scene's depth: distance " + std::to_string (before) + " -> " +
                    std::to_string (camera.Distance ()));
}

bool FollowProjectionToggle (Camera& camera, const HudState& hud, bool& last)
{
    if (hud.orthographic == last)
        return false;
    last = hud.orthographic;
    // False when the camera is already so: a preset or the plan's top view set it, the toggle followed.
    const bool switched = camera.IsOrthographic () != hud.orthographic;
    camera.SwitchProjection (hud.orthographic);
    if (switched) {
        const float before = camera.Distance ();
        // ⚠️ PARALLEL DEPTH IS THE DISTANCE: switched close to a large model, the far half would be
        // cut off. The eye backs off along its line of sight; the picture does not change.
        const bool covered = camera.CoverBoundsInDepth ();
        ArchVizLog ("viewer projection: " + DescribeView (camera) +
                    (covered ? " (backed off from " + std::to_string (before) + " to hold the model in depth)" : ""));
    }
    return true;
}

void DrawPlanCut (DiligentScene& scene, Diligent::IDeviceContext* context, const HudState& hud, bool blanked,
                  const float viewProj[16], uint32_t width, uint32_t height, uint32_t colorFormat, uint32_t depthFormat)
{
    if (!hud.planMode || !hud.planCutShown || blanked)
        return;
    StorySliceLayer::DrawParams params;
    params.widthPixels = hud.planCutWidthPixels;
    params.rgba = hud.planCutRgba;
    params.fillRgba = hud.planCutFillRgba;
    params.drawFill = hud.planCutFill;
    // Solid where something is in front of it as where nothing is: above the cut, nothing is --
    // and a turned view still reads the cut whole.
    params.occluded = OccludedStyle::Solid;
    scene.DrawPlanCut (context, viewProj, width, height, colorFormat, depthFormat, params);
}

} // namespace archviz
} // namespace geomsrv
