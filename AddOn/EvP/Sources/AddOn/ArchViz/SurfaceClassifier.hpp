// ArchViz/SurfaceClassifier — decide what a surface IS from its numbers alone.
//
// ⚠️ WHY THIS EXISTS. Archicad hands the renderer three legacy Blinn-Phong
// channels — transparency, shine and specular reflection — plus two colours.
// It has NO index of refraction, NO refraction model and NO metalness flag. A
// PBR shader needs exactly those, so something has to bridge the gap, and the
// only honest bridge is to work out what the surface IS and then describe it
// properly. That is this file. `SurfaceRoughness` in MaterialTable.hpp is the
// interim single-channel version; this is the signal that lets F0 stop being a
// fixed dielectric 0.04.
//
// ⚠️ NAMES MUST NEVER DECIDE A SURFACE'S TYPE. A STANDING USER CONSTRAINT with
// two independent reasons, either sufficient:
//   * a name can simply be WRONG. `Dažai - A3_A - Klinkeris rudas` is authored
//     as paint and describes brick;
//   * the names are Lithuanian (`Stiklas` = glass, `Metalas` = metal, `Betonas`
//     = concrete, `Plytos` = brick, `Dažai` = paint). An English keyword rule is
//     not merely incomplete here, it matches NOTHING — and the next project may
//     be in a third language.
// So: no substring matching, no keyword lists, no "glass"/"stiklas" tables.
// THE TEXTURE NAME IS A NAME LIKE ANY OTHER and is equally forbidden.
// `SurfaceMaterial::name` is deliberately never read below. The test file uses
// names only to CHECK these rules against the real template, which is the one
// legitimate use: a check that runs offline and decides nothing at runtime.
//
// ⚠️ `materialType` IS NOT USED EITHER, and not because it was overlooked. The
// official API's `SurfaceAttribute.materialType` has exactly the closed set this
// file needs — General, Simple, Matte, Metal, Plastic, Glass, Glowing, Constant
// (ModelMaterial.hpp mirrors it as `ModelerAPI::Material::Type`) — and it would
// have turned this whole file into a lookup. ALL 212 SURFACES OF THIS TEMPLATE
// REPORT `General`. Nobody authored it, and a default is indistinguishable from
// unset. ⚠️ DO re-measure it per template with SurfaceTemplateDump: a template
// that DOES author it should be read directly and this file skipped.
//
// ⚠️ A MISCLASSIFIED SURFACE IS WORSE THAN AN UNCLASSIFIED ONE, because it is
// confidently wrong. Every rule below is built to have NO false positives on the
// two risky classes — Glass and Metal, the ones that move F0 and roughness far —
// at the price of leaving genuinely ambiguous surfaces alone. `Unclassified` is
// a RESULT, not a failure; see kAmbiguousSpecular for the case that earns it.
//
// Every threshold traces to a measured value from the SurfaceTemplateDump run of
// 2026-08-21 (`dumps/surface_template__20260821_115759`, 212 surface attributes,
// 64 used by the model), re-stated at each constant. NOTHING HERE IS INTUITION.

#pragma once

#include "ArchViz/MaterialTable.hpp"

namespace geomsrv {
namespace archviz {

// What the renderer needs to know. Deliberately SMALL: each member has to earn
// its place by changing how the surface is shaded, not by naming a substance.
enum class SurfaceClass {
    // The numbers contradict each other, or sit in a band where two very
    // different materials are authored identically. Shade it as the renderer
    // always has and do not pretend to know more.
    Unclassified = 0,
    // Fully transparent. Archicad templates carry these as modelling helpers
    // ("Air"); they are not glass and must not be given a glass reflection.
    Invisible,
    // Transparent dielectric: low roughness, F0 stays dielectric, but it has to
    // actually reflect the sky or it reads as flat tinted plastic.
    Glass,
    // Conductor: F0 becomes the specular COLOUR and the diffuse term goes away.
    // The one class this file exists to find, because no Archicad channel states
    // it and inferring it wrongly paints every polished floor as chrome.
    Metal,
    // Opaque dielectric with a tight highlight — gloss paint, polished stone.
    Polished,
    // Opaque dielectric with no meaningful highlight. The overwhelming majority
    // of an architectural model, and the renderer's existing behaviour.
    Matte,
};

struct SurfaceVerdict {
    SurfaceClass cls = SurfaceClass::Unclassified;
    // 0 when unclassified, otherwise how far the deciding rule is trusted.
    // ⚠️ A CONSUMER MAY RAISE THE BAR BUT MUST NOT LOWER IT: these values are
    // chosen so that 0.75 and above holds no known false positive on this
    // template, which is the only template anyone has measured.
    float confidence = 0.0f;
};

// ---- the thresholds, each with the measurement that fixed it ---------------
//
// ⚠️ UNITS ARE `SurfaceMaterial`'S, NOT THE OFFICIAL API'S, and the two differ
// by a factor of 100 on two of the four channels. The dump prints the official
// API's integers; the renderer sees ModelerAPI's. Transparency, specular and
// diffuse arrive 0..1; the SHINE FACTOR arrives 0..100 and is stored divided by
// 100 (SurfaceMaterial::shininess), so an official `shine` of 620 is a factor of
// 6.2 and a stored `shininess` of 0.062. Every comment below quotes the official
// integer, because that is what the dump shows and what a re-measure will print.

// Transparency at or above this is a modelling helper, not a material.
// MEASURED: "Air" and "Oras" are the only two surfaces at transparency 100, and
// the next value down is 69 — so this cannot reach real glass.
constexpr float kInvisibleTransparency = 0.99f;

// Below this a surface is not meaningfully see-through.
// MEASURED: the lowest authored glass is "Glass - Lamp" at transparency 14.
// Below the line sit only "Water - Wavy" at 1 and the surfaces at 0.
constexpr float kGlassTransparency = 0.10f;

// ⚠️ THE CLEANEST LINE IN THE WHOLE TEMPLATE. MEASURED: of 212 surfaces the ones
// at specular reflection >= 80 are SEVEN glasses, NINE metals and one water, and
// nothing else whatsoever. Above this line transparency alone separates glass
// from metal, and both verdicts are safe.
constexpr float kHighSpecular = 0.80f;

// Frosted and tinted glass is authored far duller. MEASURED: "Glass - Satin" and
// "Stiklas - MATINIS" are the lowest authored glasses, both at specular 40.
constexpr float kGlassSpecular = 0.35f;

// ⚠️ WHAT KEEPS FOLIAGE OUT OF THE GLASS CLASS, and the only reason the duller
// branch is safe at all. MEASURED, in the specular 35..80 band the transparent
// surfaces are glass at diffuse 0, 10, 29 and 29 — against "Foliage - Leaves
// Tree Small" at 91 and "Insulation - Bubble Wrap" at 61. Alpha-cutout foliage
// is transparent for a completely different reason and must never be handed a
// pane's reflection.
constexpr float kGlassMaxDiffuse = 0.35f;

// ⚠️ A METAL IS FULLY OPAQUE, and the glass threshold does NOT establish that.
// MEASURED: all nine metals sit at transparency 0 exactly, while "Water - Wavy"
// is authored at transparency 1 with specular 100 — below kGlassTransparency,
// so it reaches the metal rules, and a mirror everywhere except in substance.
// The opacity bar reuses `kOpaqueAlpha` from MaterialTable.hpp, which is already
// the line between the opaque and the blended pass.

// A conductor with no highlight at all is not usable as one, and several
// surfaces are authored exactly that way: "Metal - Iron", "Metalas - GELEŽIS"
// and "Metal - Copper Aged" all carry shine 0 with specular near 50. Their own
// authoring says matte, so believe it. MEASURED: every opaque surface at
// specular >= 80 carries shine >= 520, a factor of 5.2 ("Metal - Nickel").
constexpr float kMetalMinShineFactor = 5.0f;

// ⚠️ A COLOURED SPECULAR HIGHLIGHT IS THE PHYSICAL SIGNATURE OF A CONDUCTOR:
// dielectrics reflect white, metals reflect their own colour. MEASURED: the
// tinted metals span 0.098 ("Metal - Nickel") to 0.475 ("Metal - Gold" and
// "Metal - Bronze"). ⚠️ TINT ALONE IS NOT ENOUGH AND MUST NEVER BE USED ALONE —
// Archicad authors a tinted specular colour on brick, wood, leather and stone
// too, and "Plytos - SENDINTOS MARGOS" reaches 0.400. It is the CONJUNCTION with
// a real highlight and a high specular reflection that no dielectric in this
// template satisfies.
constexpr float kSpecularTint = 0.15f;
constexpr float kTintedMetalSpecular = 0.65f;

// ⚠️ THE BAND WHERE THIS FILE REFUSES TO ANSWER, and the reason `Unclassified`
// is a first-class result rather than an error. MEASURED, opaque, neutral
// specular colour, real highlight: "Metal - Steel" sits at specular 75 and
// "Paint - Glossy White" at 69. A metal and a paint, six points apart, with no
// other channel separating them. Guessing here would render gloss-white walls as
// steel in every project that uses that surface.
constexpr float kAmbiguousSpecular = 0.65f;

// Polished versus matte is a roughness question, so it is asked in roughness
// terms: this factor is exactly where `SurfaceRoughness` crosses 0.5, because
// sqrt(2 / (6 + 2)) == 0.5. One convention, not two.
constexpr float kPolishedShineFactor = 6.0f;

// ⚠️ PURE, AND IT HAS TO STAY PURE: no Diligent, no ACAPI, no ModelerAPI, no
// I/O. That is what lets the offline suite run it over the real dumped template
// instead of rebuilding the .apx once per guessed threshold.
SurfaceVerdict ClassifySurface (const SurfaceMaterial& material);

// For logs and the dump probe. Never parsed back — this is not a serialisation.
const char* SurfaceClassName (SurfaceClass cls);

// ---- RE51.B4: the PBR description a class earns ---------------------------
//
// ⚠️ THIS IS WHERE ARCHICAD'S MISSING CHANNELS ARE FILLED IN, and it is the
// only place allowed to invent a number. `SurfaceMaterial` carries what Archicad
// stores; `SurfacePreset` carries what a PBR shader needs and Archicad never
// had — metalness above all. The classifier decides WHICH surface gets which
// treatment; this decides WHAT that treatment is.
struct SurfacePreset {
    // 0 or 1. ⚠️ NEVER INFERRED FROM SHININESS — that is the standing rule this
    // whole track exists to honour, because shininess-as-metalness paints every
    // polished floor and every window pane as chrome. It comes from the
    // classifier's verdict and from nothing else.
    float metallic = 0.0f;

    // The GGX roughness the range is drawn with.
    float roughness = 1.0f;

    // The DIELECTRIC specular level, 0..1, which the shader maps to F0 through
    // Unreal's F0 = 0.08 * reflectance. Ignored when `metallic` is 1, where F0
    // becomes the base colour instead — the glTF/DiligentFX metallic-roughness
    // convention (DiligentFX/Shaders/PBR/public/PBR_Shading.fxh:440,
    // `Reflectance0 = lerp(float3(f0,f0,f0), BaseColor, Metallic)`).
    float reflectance = 0.5f;
};

// ---- RE51.B4/B2: the substance's share of the preset ------------------------
//
// ⚠️ THE SUBSTANCE ONLY SPEAKS WHERE THE SURFACE IS SILENT, and this constant is
// the line between the two. MEASURED on the real project: nearly every painted
// surface sits at 0.72-0.8% `shining`, which is not a finish -- it is the value
// a surface has when nobody ever authored one. The genuinely authored finishes
// are an order of magnitude away (11, 18, 34, 38.56 and 79.52 percent, on the
// handful of glass and metal surfaces). So below this bar the shininess channel
// carries NO information, and substituting the substance's plausible default
// replaces nothing; above it, the author said something and it is believed.
//
// This is what stops the presets from being an override. A varnished timber
// floor authored at 40% shine keeps its gloss; the hundred untouched surfaces
// on concrete, earth and plasterboard stop all rendering with the identical
// roughness 0.99 that made Realistic look the same as Fast.
constexpr float kUnauthoredShine = 0.02f;

// ⚠️ PURE, like the classifier, and tested over the same real template. It reads
// `material.substance` as well as the numeric channels; a table built before
// that field existed carries `Unknown` and gets exactly the old behaviour.
SurfacePreset PresetFor (const SurfaceMaterial& material);

} // namespace archviz
} // namespace geomsrv
