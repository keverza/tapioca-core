#ifndef EVP_ARCHVIZ_CLAYLOOK_HPP
#define EVP_ARCHVIZ_CLAYLOOK_HPP

// ArchViz/ClayLook -- the clay (white model) material override for diagrams
// (issue #14 P2).
//
// A clay view replaces every surface's colour and finish with one neutral
// material, so a massing scheme reads by its form, its shadows and its ambient
// occlusion rather than by whatever the project's surfaces happen to be painted.
// It is the reference viewer's "arctic" look and DiligentFX's
// `DebugViewType::WhiteBaseColor`, done here on the CPU per draw range.
//
// ⚠️ A MATERIAL OVERRIDE, NOT A PRESET OF EXISTING KNOBS. The Architecture
// preview only moves AO and the background; it leaves every surface its own
// colour. Clay is the one thing those knobs cannot express, so it is a separate
// switch the previews can turn on.
//
// ⚠️ CPU-SIDE PER RANGE, NO SHADER CHANGE. Model elements carry white vertex
// colours (DiligentSceneGeometry writes 0xffffffff), so the per-range
// `baseColor` IS the surface colour and replacing it is complete. Both upload
// sites -- the forward pass and the G-buffer pass that feeds SSR/AO -- call the
// same resolver, so the two can never disagree about what a surface is.
//
// ⚠️ GLASS IS A CHOICE, NOT AN ACCIDENT. With `keepGlass` on, a translucent
// range keeps its opacity and its glass finish and only loses its tint, so the
// building still reads as glazed; off, every range becomes opaque clay, which is
// the solid-massing reading. Opacity decides the opaque/blended pass partition,
// so callers must partition on the RESOLVED alpha, never on the material's.
//
// PURE: no Diligent, no ACAPI, no I/O -- the offline suite runs it directly.

#include "ArchViz/MaterialTable.hpp"
#include "ArchViz/SurfaceClassifier.hpp"

#include <cstdint>

namespace geomsrv {
namespace archviz {

struct ClayLook {
    bool enabled = false;
    // LINEAR reflectance, like SurfaceMaterial. The default is sRGB #E6E3DE, a
    // warm off-white: pure white clips under a sunlit tone curve and loses the
    // very edges the view exists to show.
    float r = 0.7912f, g = 0.7681f, b = 0.7304f;
    // A matte, slightly sheened finish: rough enough that no reflection reads as
    // a material, smooth enough that the sun still finds the curvature.
    float roughness = 0.8f;
    bool keepGlass = true;
};

// What one draw range is drawn with, after any override.
struct DrawnSurface {
    float r = 1.0f, g = 1.0f, b = 1.0f, alpha = 1.0f;
    float roughness = 1.0f;
    float reflectance = 0.5f;
    float metallic = 0.0f;
};

// The material and its classifier preset, passed through when clay is off.
DrawnSurface ResolveDrawnSurface (const SurfaceMaterial& material, const SurfacePreset& preset, const ClayLook& clay);

// The opacity a range is drawn with -- what every opaque/translucent partition
// (shadow casters, the AO depth prepass, the blended pass) must read while clay
// may turn panes solid. Equal to `ResolveDrawnSurface (...).alpha`.
float DrawnAlpha (const SurfaceMaterial& material, const ClayLook& clay);

// The sun study's display model (issue #230): matte white, roughness 1, dielectric, and
// every pane opaque so translucent study colours cannot stack by draw order. Expressed
// as a clay look so the forward pass, the G-buffer, the shadow casters and the AO depth
// prepass all draw the SAME surface while a study is shown -- before #230 only the
// forward pass knew about it.
ClayLook StudyModelLook ();

// The HUD stores colours as sRGB 0xRRGGBBAA; the renderer wants linear.
ClayLook ClayLookFromHud (bool enabled, uint32_t srgbRgba, float roughness, bool keepGlass);

// Rec. 709 luminance of the clay colour -- what auto exposure must assume the
// mean albedo is while clay replaces the pool.
float ClayLuminance (const ClayLook& clay);

} // namespace archviz
} // namespace geomsrv

#endif
