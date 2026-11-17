// ArchViz/ClayLook -- see the header.

#include "ArchViz/ClayLook.hpp"

#include "ArchViz/ColourSpace.hpp"

#include <algorithm>

namespace geomsrv {
namespace archviz {

DrawnSurface ResolveDrawnSurface (const SurfaceMaterial& material, const SurfacePreset& preset, const ClayLook& clay)
{
    DrawnSurface out;
    if (!clay.enabled) {
        out.r = material.r;
        out.g = material.g;
        out.b = material.b;
        out.alpha = material.alpha;
        out.roughness = preset.roughness;
        out.reflectance = preset.reflectance;
        out.metallic = preset.metallic;
        return out;
    }

    out.r = clay.r;
    out.g = clay.g;
    out.b = clay.b;
    const bool glass = clay.keepGlass && material.alpha < kOpaqueAlpha;
    if (glass) {
        // The pane keeps what makes it glass -- its opacity and its finish -- and
        // loses only its tint. ⚠️ NOT METALLIC EVEN IF CLASSIFIED SO: a metallic
        // F0 is the base colour, which would turn a tinted pane into clay chrome.
        out.alpha = material.alpha;
        out.roughness = preset.roughness;
        out.reflectance = preset.reflectance;
        out.metallic = 0.0f;
        return out;
    }
    out.alpha = 1.0f;
    out.roughness = std::clamp (clay.roughness, 0.0f, 1.0f);
    out.reflectance = 0.5f;
    out.metallic = 0.0f;
    return out;
}

float DrawnAlpha (const SurfaceMaterial& material, const ClayLook& clay)
{
    if (!clay.enabled || (clay.keepGlass && material.alpha < kOpaqueAlpha))
        return material.alpha;
    return 1.0f;
}

ClayLook ClayLookFromHud (bool enabled, uint32_t srgbRgba, float roughness, bool keepGlass)
{
    ClayLook clay;
    clay.enabled = enabled;
    clay.r = SrgbToLinear (float ((srgbRgba >> 24) & 0xFFu) / 255.0f);
    clay.g = SrgbToLinear (float ((srgbRgba >> 16) & 0xFFu) / 255.0f);
    clay.b = SrgbToLinear (float ((srgbRgba >> 8) & 0xFFu) / 255.0f);
    clay.roughness = std::clamp (roughness, 0.0f, 1.0f);
    clay.keepGlass = keepGlass;
    return clay;
}

float ClayLuminance (const ClayLook& clay)
{
    return 0.2126f * clay.r + 0.7152f * clay.g + 0.0722f * clay.b;
}

} // namespace archviz
} // namespace geomsrv
