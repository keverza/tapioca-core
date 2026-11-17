// ArchViz/DiligentViewportLook -- the HUD's surface look applied to the scene each frame:
// the Realistic grading and the clay override (issue #14 P2, ClayLook.hpp). Declared in
// DiligentViewportSupport.hpp; its own translation unit because that one is at its size cap.

#include "ArchViz/DiligentViewportSupport.hpp"

#include "ArchViz/ClayLook.hpp"
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentScene.hpp"

namespace geomsrv {
namespace archviz {

void ApplySurfaceLook (DiligentScene& scene, const HudState& hud, bool overlay)
{
    scene.SetGrading (hud.exposure, hud.reflectance, hud.roughnessBias);
    // ⚠️ WITHHELD ON THE OVERLAY, like the sky background: over Archicad's own 3D window the
    // picture is compared against Archicad's shading, so it must keep the project's materials.
    scene.SetClayLook (ClayLookFromHud (hud.clay && !overlay, hud.clayRgba, hud.clayRoughness, hud.clayKeepGlass));
}

} // namespace archviz
} // namespace geomsrv
