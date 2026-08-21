#include "ArchViz/SurfaceClassifier.hpp"

#include <algorithm>

namespace geomsrv {
namespace archviz {

namespace {

// How far the specular colour departs from neutral grey. ⚠️ MAX MINUS MIN, NOT
// a saturation formula: it is the plain statement "these three numbers are not
// the same", which is what "the highlight is coloured" means, and it needs no
// colour space to be true. A saturation would additionally depend on how BRIGHT
// the specular colour is, and a dark neutral grey would then read as tinted.
float SpecularTint (const SurfaceMaterial& m)
{
    const float hi = (std::max) ((std::max) (m.specularR, m.specularG), m.specularB);
    const float lo = (std::min) ((std::min) (m.specularR, m.specularG), m.specularB);
    return hi - lo;
}

} // namespace

SurfaceVerdict ClassifySurface (const SurfaceMaterial& m)
{
    // ⚠️ THE STRUCT STORES ALPHA, THE DUMP PRINTS TRANSPARENCY, AND THEY ARE
    // OPPOSITES. `ExtractionEnvironment` already performed the flip (1 = OPAQUE
    // here, 1 = INVISIBLE there); undoing it once, here, keeps every threshold
    // in this file readable against the dump it was measured from.
    const float transparency = 1.0f - m.alpha;

    // The shine FACTOR, 0..100, as the DevKit documents it. `shininess` carries
    // it divided by 100 — see the header's units note and SurfaceRoughness.
    const float shineFactor = m.shininess * 100.0f;

    // ---- fully transparent: a modelling helper, not a material -------------
    if (transparency >= kInvisibleTransparency)
        return { SurfaceClass::Invisible, 1.00f };

    // ---- see-through: glass, or something transparent for another reason ---
    if (transparency >= kGlassTransparency) {
        // Above kHighSpecular the only transparent surfaces in the template are
        // glass, so transparency plus specular settles it on its own.
        if (m.specular >= kHighSpecular)
            return { SurfaceClass::Glass, 0.95f };

        // Frosted and tinted glass is duller, and here the diffuse channel is
        // load-bearing: it is what separates a satin pane from a cut-out leaf.
        if (m.specular >= kGlassSpecular && m.diffuse <= kGlassMaxDiffuse)
            return { SurfaceClass::Glass, 0.75f };

        // ⚠️ TRANSPARENT AND NOT GLASS-SHAPED. Foliage and bubble wrap land
        // here. Falling through to the opaque rules below would be wrong — they
        // assume opacity — and calling it glass would be worse, so it stops.
        return { SurfaceClass::Unclassified, 0.0f };
    }

    // ---- opaque, with a real highlight: the only place metal can be ---------
    //
    // ⚠️ A METAL MUST BE FULLY OPAQUE, AND `< kGlassTransparency` IS NOT ENOUGH
    // TO SAY SO. "Water - Wavy" is authored at transparency 1 with specular 100
    // and shine 700 — a hair's breadth of transparency, a mirror's reflectance,
    // and emphatically a dielectric. It clears the glass threshold by 9 points
    // and would otherwise be called chrome. MEASURED: every one of the nine
    // metals in the template is at transparency 0 exactly.
    //
    // The bar is `kOpaqueAlpha` rather than a new constant because that is
    // already the line between the opaque and the blended pass, and a surface
    // the renderer blends is not one to hand a conductor's F0.
    const bool fullyOpaque = m.alpha >= kOpaqueAlpha;

    if (fullyOpaque && shineFactor >= kMetalMinShineFactor) {
        if (m.specular >= kHighSpecular)
            return { SurfaceClass::Metal, 0.90f };

        // A coloured highlight on a surface that also reflects strongly. Neither
        // half is sufficient alone; see kSpecularTint.
        if (SpecularTint (m) >= kSpecularTint && m.specular >= kTintedMetalSpecular)
            return { SurfaceClass::Metal, 0.80f };

        // ⚠️ THE REFUSAL, AND IT IS DELIBERATE. Opaque, neutral, strongly
        // specular, real highlight — and in this template that description fits
        // both "Metal - Steel" and "Paint - Glossy White". There is no channel
        // left to ask, so this reports that it does not know instead of picking.
        if (m.specular >= kAmbiguousSpecular)
            return { SurfaceClass::Unclassified, 0.0f };
    }

    // ---- opaque dielectric: the ordinary case, and most of a building -------
    if (shineFactor >= kPolishedShineFactor)
        return { SurfaceClass::Polished, 0.60f };

    return { SurfaceClass::Matte, 0.60f };
}

const char* SurfaceClassName (SurfaceClass cls)
{
    switch (cls) {
        case SurfaceClass::Invisible:
            return "invisible";
        case SurfaceClass::Glass:
            return "glass";
        case SurfaceClass::Metal:
            return "metal";
        case SurfaceClass::Polished:
            return "polished";
        case SurfaceClass::Matte:
            return "matte";
        case SurfaceClass::Unclassified:
            break;
    }
    // ⚠️ NO `default:` ABOVE, ON PURPOSE: adding a class then makes the compiler
    // point at this switch instead of silently returning "unclassified" for it.
    return "unclassified";
}

} // namespace archviz
} // namespace geomsrv
