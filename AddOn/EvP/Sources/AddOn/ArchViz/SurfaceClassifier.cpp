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

// ---- RE51.B4 --------------------------------------------------------------

// The dielectric specular level that reproduces F0 = 0.04, which is the
// Fresnel reflectance of an IOR-1.5 dielectric at normal incidence -- window
// glass, and the value every PBR renderer uses as its dielectric default.
// ⚠️ 0.5, NOT 0.04: the shader maps reflectance to F0 as 0.08 * reflectance
// (Unreal's remapping), so 0.5 IS 0.04. Writing 0.04 here would give F0 0.0032.
constexpr float kDielectricReflectance = 0.5f;

// The PBR description a SUBSTANCE earns, for a surface whose own finish channel
// carries nothing (see kUnauthoredShine). Applied only in that case, and only
// where the optical classifier has already declined to answer.
//
// ⚠️ EVERY NUMBER HERE IS INVENTED, WHICH IS EXACTLY WHY EACH ONE IS JUSTIFIED
// AGAINST A PHYSICAL REFERENCE RATHER THAN CHOSEN BY EYE. Archicad stores no
// roughness and no IOR; there is nothing to measure, so the alternative to a
// referenced default is an unreferenced one.
//
// Reflectance is the DIELECTRIC level the shader maps through F0 = 0.08 * r, so
// 0.5 is F0 = 0.04, the IOR-1.5 dielectric every PBR renderer defaults to.
void ApplySubstancePreset (Substance substance, SurfacePreset& preset)
{
    switch (substance) {
        case Substance::Concrete:
            // Cast concrete is a rough dielectric with an ordinary IOR near 1.5.
            // Not 1.0: even a raw finish has enough specular sheen at grazing
            // angles to catch the sky, and a roughness of exactly 1 removes it.
            preset.roughness = 0.85f;
            break;

        case Substance::Earth:
            // Soil, gravel and topsoil: the roughest thing on a site, and a
            // LOWER specular level than mineral concrete because the organic
            // fraction has an IOR nearer 1.4 (F0 ~ 0.028).
            preset.roughness = 0.95f;
            preset.reflectance = 0.35f;
            break;

        case Substance::Wood:
            // Bare or lightly finished timber. ⚠️ NOT GLOSSY: a lacquered floor
            // is authored with a real shine and therefore never reaches this
            // branch at all, which is the whole design of kUnauthoredShine.
            preset.roughness = 0.65f;
            break;

        case Substance::Plastic:
            // Polymers sit at IOR 1.5-1.6, so slightly ABOVE the dielectric
            // default (F0 ~ 0.048), and unfinished plastic parts read as
            // semi-gloss rather than matte.
            preset.roughness = 0.45f;
            preset.reflectance = 0.60f;
            break;

        case Substance::Metal:
            // ⚠️ THE ONE SUBSTANCE THAT CHANGES THE LIGHTING MODEL RATHER THAN
            // ITS PARAMETERS, and the one case where the building material
            // genuinely knows something the surface does not. A powder-coated
            // steel balustrade is authored with a flat paint surface; only the
            // material behind it says conductor.
            preset.metallic = 1.0f;
            preset.roughness = 0.35f;
            break;

        case Substance::Glass:
            // ⚠️ DELIBERATELY NOTHING. A GLASS building material reaching this
            // branch means the SURFACE in front of it is opaque and unpolished
            // -- a spandrel panel, or the frame captured with the pane. Trusting
            // the substance here would render solid panels as windows, which is
            // the confidently-wrong failure this whole track refuses to make.
            // Real glazing is caught optically by ClassifySurface, from
            // transparency, and never gets here.
            break;

        case Substance::Unknown:
            break;
    }
}

SurfacePreset PresetFor (const SurfaceMaterial& m)
{
    SurfacePreset preset;
    preset.roughness = SurfaceRoughness (m);
    preset.reflectance = m.specular;

    switch (ClassifySurface (m).cls) {
        case SurfaceClass::Metal:
            // ⚠️ THE ONE LINE THIS ENTIRE TRACK EXISTS TO WRITE. Archicad has no
            // metalness channel, so until the classifier could say "this surface
            // is a conductor" every metal in every project was shaded as a
            // dielectric reflecting 4% of the sky. F0 becomes the base colour in
            // the shader and the diffuse lobe goes away, which is what makes a
            // metal look like one.
            preset.metallic = 1.0f;
            break;

        case SurfaceClass::Glass:
            // ⚠️ THE MEASURED SPECULAR IS DISCARDED HERE, ON PURPOSE. This
            // project authors its glass at specular 100, which the shader would
            // map to F0 = 0.08 -- twice the physical reflectance of glass. The
            // physical reference is the one number about glass nobody disputes:
            // IOR 1.5 gives F0 = 0.04. What makes a pane read as glass is not a
            // fattened head-on reflection, it is the FRESNEL RISE toward grazing
            // angles, and that arrives correctly from the split-sum term once F0
            // is right.
            preset.reflectance = kDielectricReflectance;
            break;

        case SurfaceClass::Invisible:
        case SurfaceClass::Polished:
        case SurfaceClass::Matte:
        case SurfaceClass::Unclassified:
            // ⚠️ THE OPTICAL REFUSAL STILL CHANGES NOTHING BY ITSELF. What
            // follows is a SECOND, INDEPENDENT source of evidence -- the
            // building material behind the element -- and it is consulted only
            // here, where the surface's own channels have already failed to
            // answer. A surface the optical classifier NAMED is never
            // second-guessed by the substance: transparency and specular
            // reflection are genuine optical measurements of the thing being
            // drawn, and the building material behind a pane of glass is often
            // the mullion's steel.
            //
            // ⚠️ AND ONLY WHERE THE FINISH CHANNEL IS UNAUTHORED. See
            // kUnauthoredShine: above that bar the author said something about
            // this surface and it is believed, substance or no substance.
            if (m.shininess <= kUnauthoredShine)
                ApplySubstancePreset (m.substance, preset);
            break;
    }
    return preset;
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
