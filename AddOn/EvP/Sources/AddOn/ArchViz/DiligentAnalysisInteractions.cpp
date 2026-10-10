#include "ArchViz/DiligentViewportSupport.hpp"
#include "ArchViz/Camera.hpp"
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/MatrixMath.hpp"
#include <cmath>

namespace geomsrv::archviz {

void ServiceViewpointInput (const Camera& camera, HudState& hud, const InputSnapshot& input, uint32_t width,
                            uint32_t height)
{
    // Resolve the gumball BEFORE navigation consumes a left-button transition.
    // Projection uses the same stable camera, never the scene's TAA jitter.
    float view[16], projection[16], viewProj[16];
    camera.GetViewMatrix (view);
    camera.GetProjMatrix (projection, height > 0 ? float (width) / float (height) : 1.0f);
    Multiply (viewProj, view, projection);
    float origin[3], direction[3];
    camera.CursorRay (CursorTargetX (input, width), CursorTargetY (input, height), width, height, origin, direction);
    viewerhud::ServiceViewpointInteractions (hud, input, viewProj, width, height, origin, direction);
}

void ServiceCursorAnalysis (const Camera& camera, HudState& hud, const DiligentScene& scene, const InputSnapshot& input,
                            uint32_t width, uint32_t height)
{
    hud.cursorX = input.x;
    hud.cursorY = input.y;
    hud.cursorGroundValid = false;
    if (!input.inside || height == 0 || width == 0)
        return;
    float origin[3], direction[3];
    camera.CursorRay (CursorTargetX (input, width), CursorTargetY (input, height), width, height, origin, direction);
    viewerhud::ServiceAnalysisInteractions (hud, scene, input, origin, direction);
    viewerhud::PlaceViewpointFromPick (hud, scene, input, origin, direction);
    if (std::abs (direction[2]) > 1e-6f) {
        const float t = -origin[2] / direction[2];
        if (t > 0.0f) {
            for (int axis = 0; axis < 3; ++axis)
                hud.cursorGround[axis] = origin[axis] + direction[axis] * t;
            hud.cursorGround[2] = 0.0f;
            hud.cursorGroundValid = true;
        }
    }
}

} // namespace geomsrv::archviz
