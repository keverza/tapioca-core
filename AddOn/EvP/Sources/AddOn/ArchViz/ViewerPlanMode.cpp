// ArchViz/ViewerPlanMode -- see the header.

#include "ArchViz/ViewerPlanMode.hpp"

#include "ArchViz/Camera.hpp"
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/DiligentViewportSupport.hpp" // ApplyArchicadCamera
#include "ArchViz/StorySliceLayer.hpp"

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

bool NavigateViewer (Camera& camera, HudState& hud, const InputSnapshot& input, uint32_t width, uint32_t height)
{
    bool moved = false;
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
    return camera.ApplyInput (input, hud.wantsMouse, width, height) || moved;
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
