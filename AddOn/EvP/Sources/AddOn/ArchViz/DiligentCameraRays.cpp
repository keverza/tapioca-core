#include "ArchViz/DiligentCameraRays.hpp"

#include "ArchViz/Camera.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/MatrixMath.hpp"

#include <cmath>

namespace geomsrv::archviz {

void SetDiligentCameraRays (DiligentScene& scene, const Camera& camera, uint32_t width, uint32_t height)
{
    float eyeWorld[3];
    float targetWorld[3];
    camera.GetEyePosition (eyeWorld);
    camera.GetTarget (targetWorld);
    float forward[3];
    float right[3];
    float up[3];
    CameraBasis (eyeWorld, targetWorld, forward, right, up);
    float rayRight[3] = { 0.0f, 0.0f, 0.0f };
    float rayUp[3] = { 0.0f, 0.0f, 0.0f };
    if (camera.IsPerspective () && height > 0) {
        constexpr float kPi = 3.14159265358979323846f;
        const float halfV = std::tan (camera.FovDegreesVertical () * 0.5f * (kPi / 180.0f));
        const float halfH = halfV * (float (width) / float (height));
        for (int axis = 0; axis < 3; ++axis) {
            // CameraBasis's right is screen-left for the view matrix.
            rayRight[axis] = -right[axis] * halfH;
            rayUp[axis] = up[axis] * halfV;
        }
    }
    scene.SetCameraRays (rayRight, rayUp, forward);
}

} // namespace geomsrv::archviz
