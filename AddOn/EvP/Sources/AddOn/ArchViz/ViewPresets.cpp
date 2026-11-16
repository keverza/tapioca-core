// ArchViz/ViewPresets -- see the header.

#include "ArchViz/ViewPresets.hpp"

#include "ArchViz/Camera.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv {
namespace archviz {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// A view's weights on the model's right, front and top sides; the direction is their sum.
struct Weights {
    float right, front, top;
};

bool WeightsFor (ViewPreset preset, Weights& out)
{
    switch (preset) {
        case ViewPreset::IsometricNE:
            out = { 1.0f, -1.0f, 1.0f };
            return true;
        case ViewPreset::IsometricNW:
            out = { -1.0f, -1.0f, 1.0f };
            return true;
        case ViewPreset::IsometricSE:
            out = { 1.0f, 1.0f, 1.0f };
            return true;
        case ViewPreset::IsometricSW:
            out = { -1.0f, 1.0f, 1.0f };
            return true;
        case ViewPreset::Dimetric:
            // 1 : 1 : 1/2 -- the squares of the direction's parts are 1/9, 7/9 and 1/9.
            out = { 1.0f, std::sqrt (7.0f), 1.0f };
            return true;
        case ViewPreset::Trimetric:
            // 1 : 0.9 : 0.5 -- the squares are 3/103, 78/103 and 22/103.
            out = { std::sqrt (3.0f), std::sqrt (78.0f), std::sqrt (22.0f) };
            return true;
        default:
            return false;
    }
}

} // namespace

const char* ViewPresetName (ViewPreset preset)
{
    switch (preset) {
        case ViewPreset::IsometricNE:
            return "Isometric NE";
        case ViewPreset::IsometricNW:
            return "Isometric NW";
        case ViewPreset::IsometricSE:
            return "Isometric SE";
        case ViewPreset::IsometricSW:
            return "Isometric SW";
        case ViewPreset::Dimetric:
            return "Dimetric";
        case ViewPreset::Trimetric:
            return "Trimetric";
        case ViewPreset::Top:
            return "Top";
        default:
            return "none";
    }
}

bool ViewPresetDirection (ViewPreset preset, float northDegrees, float out[3])
{
    Weights w;
    if (!WeightsFor (preset, w))
        return false;
    const float n = northDegrees * (kPi / 180.0f);
    const float right[2] = { std::sin (n), -std::cos (n) };
    const float front[2] = { -std::cos (n), -std::sin (n) };
    const float d[3] = { w.right * right[0] + w.front * front[0], w.right * right[1] + w.front * front[1], w.top };
    const float length = std::sqrt (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; ++i)
        out[i] = d[i] / length;
    return true;
}

float TopDownRotationForNorth (float northDegrees)
{
    // SetTopDown's screen up is (-sin r, cos r); north is (cos n, sin n).
    return northDegrees * (kPi / 180.0f) - 0.5f * kPi;
}

const char* ViewPresetNote (ViewPresetOutcome outcome)
{
    switch (outcome) {
        case ViewPresetOutcome::HeldToPlan:
            return "held to the plan: free the orbit first";
        case ViewPresetOutcome::DirectionOnly:
            return "no model yet: direction only, not framed";
        default:
            return nullptr;
    }
}

ViewPresetOutcome ApplyViewPreset (Camera& camera, ViewPreset preset, float northDegrees, bool planMode, bool planOrbit,
                                   const float* frameMin, const float* frameMax, const float* sceneMin,
                                   const float* sceneMax, float aspect)
{
    float direction[3];
    const bool top = preset == ViewPreset::Top;
    if (!top && !ViewPresetDirection (preset, northDegrees, direction))
        return ViewPresetOutcome::None;
    if (planMode && !planOrbit)
        return ViewPresetOutcome::HeldToPlan;
    if (top && planMode && camera.HasPlanHome ()) {
        camera.ReturnToPlan ();
        return ViewPresetOutcome::PlanHome;
    }

    // ⚠️ THE PROJECTION FIRST, THROUGH THE ONE CONVERSION (Camera::SwitchProjection): with no box
    // to frame, the extent the perspective showed at the target is the extent the preset keeps.
    camera.SwitchProjection (true);
    if (top)
        camera.SetTopDown (TopDownRotationForNorth (northDegrees));
    else
        camera.SetOrbit (std::atan2 (direction[1], direction[0]), std::asin (direction[2]));

    const bool framed = frameMin != nullptr && frameMax != nullptr;
    if (framed) {
        camera.FrameBounds (frameMin, frameMax);
        // FrameBounds fits the box's sphere to the surface's HEIGHT; a surface taller than wide
        // shows less across than up.
        if (aspect > 1e-3f && aspect < 1.0f)
            camera.SetOrthographic (true, camera.OrthoHalfHeightMetres () / aspect);
    }
    // ⚠️ FrameBounds REMEMBERED THE BOX IT FRAMED. The scene's stays the one a fit and the depth
    // range mean, or a preset over a selection would leave "zoom to fit" fitting the selection.
    if (sceneMin != nullptr && sceneMax != nullptr)
        camera.SetBounds (sceneMin, sceneMax);
    camera.CoverBoundsInDepth ();
    return framed ? ViewPresetOutcome::Framed : ViewPresetOutcome::DirectionOnly;
}

} // namespace archviz
} // namespace geomsrv
