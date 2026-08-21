#ifndef EVP_ARCHVIZ_MATERIALTABLE_HPP
#define EVP_ARCHVIZ_MATERIALTABLE_HPP

// The 3D model's surface pool, resolved to something a shader can use.
//
// `Mesh::triMaterial` carries ONE INTEGER per triangle and nothing else: it is
// `ModelerAPI::AttributeIndex::GetIndex()` on the polygon's material, i.e. an
// index into THE MODEL'S OWN pool (the surfaces the model actually uses,
// renumbered — NOT Archicad attribute indices; see
// NativeCommands/ModelAppearanceCommands.cpp's warning, which is the other
// consumer of the same numbers). Without this table those integers are an opaque
// group key and every surface renders white, which is where Phase 6 stood.
//
// ⚠️ ONE TABLE PER SCENE, NOT PER ELEMENT. The pool is a property of the model,
// so it is extracted once per rebuild and handed over once. Copying a colour
// into every element's upload would multiply it by the element count and, worse,
// let two elements disagree about what material 7 is after a partial refresh.
//
// ⚠️ NO bgfx, NO bx, NO ACAPI — the same rule as MeshGroups and SceneCmdQueue,
// and for the same reason: a wrong lookup here is INVISIBLE (the building
// renders, in the wrong colours, which reads as a styling choice), so it has to
// be reachable by tests/cpp. The producer converts ModelerAPI::Material into
// these plain structs; nothing modeler-shaped crosses this header.
//
// ⚠️ COLOURS ARE 0..1 FLOATS, not 0..255 bytes. That is ModelerAPI's own
// convention (`GetSurfaceColor`), passed through rather than rescaled, so the
// numbers here can be diffed against EvP.GetModelMaterials' output directly.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {

struct SurfaceMaterial {
    // The pool index this describes — the value that appears in
    // `Mesh::triMaterial` and therefore in `MaterialRange::material`.
    int32_t index = 0;

    float r = 1.0f, g = 1.0f, b = 1.0f;
    // ⚠️ OPACITY, NOT TRANSPARENCY, AND THE FLIP IS THE POINT OF THE FIELD
    // NAME. ModelerAPI reports `GetTransparency()` where 1 means FULLY
    // TRANSPARENT; a shader wants alpha, where 1 means fully opaque. Storing the
    // shader's convention here means the flip happens exactly once, in the
    // producer, instead of at every read — and a table full of invisible glass
    // is what forgetting it looks like.
    float alpha = 1.0f;

    // 0..1, ModelerAPI's `GetShining` rescaled by the producer — it reports a
    // PERCENTAGE. ⚠️ MEASURED, NOT INFERRED: SurfaceFinishProbe read the live
    // pool through EvP.GetModelMaterials, which calls the very same
    // `ModelerAPI::Material::GetShining()` unscaled (ModelAccessUtils.cpp), and
    // saw min 0 / max 79.52 over 68 surfaces. The DevKit's own `m_shine`
    // comment (UMAT.hpp, "shininess * 100, [0..10000]") describes the RAW
    // storage behind it and disagrees by a further factor of 100 — do not
    // "correct" the divisor to match that comment without re-measuring.
    float shininess = 0.0f;

    // 0..1, ModelerAPI's `GetSpecularReflection` — how much light leaves
    // specularly, as opposed to `shininess`, which is how TIGHT the highlight
    // is. ⚠️ ALREADY 0..1 AT SOURCE, unlike its neighbour above: the same probe
    // read min 0 / max 1 across 17 distinct values. Two channels with different
    // scales from one API is exactly the trap that flattened the first one, so
    // the two comments say their ranges out loud.
    //
    // 0.5 IS THE NEUTRAL VALUE, not 0 — it is the ordinary dielectric, and the
    // shader's F0 mapping is built so that 0.5 reproduces the fixed 0.04 the
    // renderer used before this channel existed.
    float specular = 0.5f;

    // 0..1, ModelerAPI's `GetDiffuseReflection`. ⚠️ CARRIED FOR THE CLASSIFIER,
    // NOT FOR THE SHADER, which gets its diffuse from the surface COLOUR. It is
    // here because it is the one channel that separates a satin pane from a
    // cut-out leaf when both are transparent and dull — see
    // SurfaceClassifier.hpp, kGlassMaxDiffuse.
    float diffuse = 0.5f;

    // 0..1 each, ModelerAPI's `GetSpecularColor`. ⚠️ THE ONLY METALNESS SIGNAL
    // ARCHICAD HAS, and it is not labelled as one: dielectrics reflect white and
    // conductors reflect their own colour, so a highlight that is NOT neutral
    // grey is the physical signature of a metal. Archicad has no metalness flag
    // and no IOR, so without this channel a conductor cannot be told from gloss
    // paint at all. ⚠️ IT IS NEVER SUFFICIENT ALONE — brick, wood and stone
    // carry tinted specular colours too; SurfaceClassifier.hpp says what it must
    // be conjoined with.
    //
    // Defaults are neutral white so a table built before this channel existed
    // reads as an ordinary dielectric rather than as a coloured metal.
    float specularR = 1.0f;
    float specularG = 1.0f;
    float specularB = 1.0f;

    std::string name;

    size_t Bytes () const { return sizeof (SurfaceMaterial) + name.capacity (); }
};

// At or above this, a surface draws in the opaque pass. Not 1.0: Archicad
// reports transparency as a double and a surface authored at "0%" can arrive as
// 0.0000001, which would put the entire building in the blended pass — every
// wall drawn without depth writes, which looks like the model turning inside
// out. Shared by the producer and the renderer so the threshold cannot differ
// between the count and the draw.
constexpr float kOpaqueAlpha = 0.995f;

// ⚠️ ARCHICAD'S SHINE IS A BLINN-PHONG EXPONENT, NOT A GLOSS FRACTION,
// and reading it as one is what left this project with no reflections at all.
//
// The DevKit says so twice, in the two places that define the channel:
// APIdefs_Attributes.h:1122 -- "The shininess factor multiplied by 100.
// [0..10000]" -- and Model3D/UMAT.hpp:45 -- "shininess * 100 [0..10000]".
// So the SHININESS FACTOR itself runs 0..100, and a "factor" in a Blinn-Phong
// model is the specular EXPONENT: 0 is perfectly diffuse, and larger is a
// tighter highlight. `SurfaceMaterial::shininess` carries that factor divided
// by 100, so the exponent is recovered by multiplying it back.
//
// The old mapping was `1 - shininess`, which reads the factor as if it were a
// linear 0..1 gloss. Measured against this project's real pool
// (SurfaceTemplateDump, 2026-08-20, 23 surfaces used by the model) that put
// EVERY surface between roughness 0.718 and 1.0 -- the glossiest surface in the
// whole building, at factor 28.18, still came out rougher than three quarters.
// Since the shader scales the environment reflection by (1-roughness)^2, that
// left the glossiest material reflecting 7.9% of the sky and the median
// material reflecting none, which is exactly the reported symptom.
//
// The conversion below is the standard exponent-to-GGX-roughness relation,
// roughness = sqrt(2 / (n + 2)), which matches the two lobes' widths. On the
// measured pool it separates the materials the way a facade actually reads:
//   factor 28.18 -> 0.257    factor 1.00 -> 0.816
//   factor 18.00 -> 0.316    factor 0.80 -> 0.845
//   factor 11.00 -> 0.392    factor 0.00 -> 1.000
constexpr float kMaxShininessFactor = 100.0f;

// Archicad exposes transparency, shine and specular reflection for a surface,
// but no IOR, no refraction and no metalness. Transparent ranges are therefore
// treated as dielectric glass for the forward preview. Keep authored shine when
// it gives a useful gloss value, but do not let a surface authored with no
// shine at all erase the reflection that makes glass read as glass.
//
// ⚠️ THIS IS A FLOOR ON GLOSS, NOT A CLASSIFIER. It cannot tell a pane from a
// tinted plastic, and it must not grow into something that tries: the material
// presets (RE51.B1) are where a surface's TYPE gets decided, from measured
// numbers rather than from names.
constexpr float kTransparentRoughnessCeiling = 0.35f;

inline float SurfaceRoughness (const SurfaceMaterial& material)
{
    // ⚠️ CLAMP THE FACTOR, NOT THE ROUGHNESS. sqrt(2/(n+2)) is already inside
    // (0..1] for every n >= 0, so the only value that needs defending is a
    // negative or out-of-spec factor arriving from the model.
    float factor = material.shininess * kMaxShininessFactor;
    // ⚠️ ALSO CATCHES NaN: the comparison is false for it, so the negation
    // is true and the surface lands on matte instead of on a mirror.
    if (!(factor > 0.0f))
        factor = 0.0f;
    else if (factor > kMaxShininessFactor)
        factor = kMaxShininessFactor;

    float roughness = std::sqrt (2.0f / (factor + 2.0f));

    if (material.alpha < kOpaqueAlpha && roughness > kTransparentRoughnessCeiling)
        roughness = kTransparentRoughnessCeiling;
    return roughness;
}

class MaterialTable final {
public:
    // Add or REPLACE the entry for `m.index`. Replacing rather than appending so
    // a pool that reports the same index twice cannot produce two answers to one
    // lookup — a duplicate is a bad read, and the last one wins quietly instead
    // of the first one winning by accident of iteration order.
    void Set (const SurfaceMaterial& m);

    // The material for a triangle's pool index. NEVER null: an index the pool
    // does not describe returns `Missing()`, a plain opaque white.
    //
    // ⚠️ WHITE, NOT MAGENTA. The tempting "loud colour for a missing material"
    // is wrong here: index 0 is ordinary (the extractor emits it for a polygon
    // with no material, and for the truncated-array case MeshGroups documents),
    // so a debug colour would paint real buildings pink. The count of misses is
    // the diagnostic instead — see `Misses()`.
    const SurfaceMaterial& Lookup (int32_t index) const;

    // Whether the pool actually describes `index`.
    //
    // ⚠️ THIS EXISTS BECAUSE Lookup MUST NOT COUNT ITS OWN MISSES. It runs once
    // per draw range per FRAME, so a miss counter inside it would climb forever
    // and report 60x whatever it meant per second — a number that looks like a
    // measurement and is a frame count. The viewer counts unmapped ranges ONCE,
    // when an element is uploaded, using this.
    bool Has (int32_t index) const;

    static const SurfaceMaterial& Missing ();

    size_t Size ()    const { return byIndex_.size (); }
    bool   IsEmpty () const { return byIndex_.empty (); }
    size_t Bytes ()   const;

    const std::vector<SurfaceMaterial>& All () const { return byIndex_; }

private:
    // A flat vector, linear-scanned. A pool has tens of entries, the lookup runs
    // once per DRAW RANGE (not per triangle, not per vertex), and a map would
    // cost an allocation per entry to save nothing measurable. Revisit only with
    // a measurement — the same standard SceneCmdQueue's mutex is held to.
    std::vector<SurfaceMaterial> byIndex_;
};

}   // namespace archviz
}   // namespace geomsrv

#endif
