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
