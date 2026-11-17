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
    // The wireframe colour applies on the overlay too: there the wire IS the picture.
    float wire[4];
    for (int k = 0; k < 4; ++k)
        wire[k] = float ((hud.wireRgba >> (24 - 8 * k)) & 0xFF) / 255.0f;
    scene.SetWireColor (wire);
    // ⚠️ WITHHELD ON THE OVERLAY, like the sky background: over Archicad's own 3D window the
    // picture is compared against Archicad's shading, so it must keep the project's materials.
    scene.SetClayLook (ClayLookFromHud (hud.clay && !overlay, hud.clayRgba, hud.clayRoughness, hud.clayKeepGlass));
    // Issue #14 P2. Withheld on the overlay for the same reason: there the lines that matter are the
    // ones that register against Archicad's own window, which is the wireframe's job.
    scene.SetTechnicalEdges (TechnicalEdgeStyleFromHud (hud.technicalEdges && !overlay, hud.technicalEdgeRgba,
                                                        hud.technicalEdgePixels, hud.technicalEdgeHidden,
                                                        hud.technicalEdgeAngle));
    // The section plane: withheld on the overlay, whose picture must match Archicad's window.
    SectionPlaneState section;
    section.enabled = hud.sectionEnabled && !overlay;
    section.axis = SectionAxis (hud.sectionAxis < 0 || hud.sectionAxis > 2 ? 2 : hud.sectionAxis);
    section.position = hud.sectionPosition;
    section.flipped = hud.sectionFlipped;
    scene.SetSectionPlane (section);
    SectionCapStyle caps;
    caps.caps = hud.sectionCaps;
    caps.fillRgba = hud.sectionCapRgba;
    caps.lineRgba = hud.sectionLineRgba;
    caps.backFaces = hud.sectionBackFaces;
    caps.hatchRgba = hud.sectionHatchRgba;
    caps.shape.lineWidth = hud.sectionLineMm * 0.001f;
    caps.shape.hatch = hud.sectionHatch;
    caps.shape.hatchSpacing = hud.sectionHatchSpacingMm * 0.001f;
    caps.shape.hatchAngleDegrees = hud.sectionHatchAngle;
    scene.SetSectionCapStyle (caps);
    // #231: withheld on the overlay -- a guest on Archicad's D3D11 device cannot trace.
    RayTracedSettings traced;
    traced.enabled = hud.rayTraced && !overlay;
    traced.maxSamples = uint32_t (hud.rayTracedMaxSamples < 1 ? 1 : hud.rayTracedMaxSamples);
    traced.bounces = uint32_t (hud.rayTracedBounces < 0 ? 0 : hud.rayTracedBounces);
    traced.reflections = hud.rayTracedReflections && !overlay;
    scene.SetRayTraced (traced);
}

} // namespace archviz
} // namespace geomsrv
