// ArchViz/ViewerPlanMode -- see the header.

#include "ArchViz/ViewerPlanMode.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/Camera.hpp"
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/DiligentViewportSupport.hpp" // ApplyArchicadCamera
#include "ArchViz/OrbitAnchor.hpp"
#include "ArchViz/StorySliceLayer.hpp"

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
    FollowOrbitAnchor (camera, hud, scene, input);
    return camera.ApplyInput (input, hud.wantsMouse || hud.viewpointOwnsMouse, width, height) || moved;
}

bool FollowProjectionToggle (Camera& camera, const HudState& hud, bool& last)
{
    if (hud.orthographic == last)
        return false;
    last = hud.orthographic;
    camera.SwitchProjection (hud.orthographic);
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
