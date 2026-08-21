#ifndef EVP_ARCHVIZ_SUBSTANCEJOIN_HPP
#define EVP_ARCHVIZ_SUBSTANCEJOIN_HPP

// ArchViz/SubstanceJoin — the arithmetic that carries a BUILDING material's
// substance across to the SURFACE the renderer actually draws.
//
// ⚠️ THIS FILE EXISTS BECAUSE BuildingMaterialSignal HAD NO CALLER. That
// classifier was measured on two real projects and is correct on 100% of 157
// building materials, and none of it reached a pixel: `ExtractionEnvironment`
// reads the ModelerAPI SURFACE pool, `Mesh::triMaterial` carries a SURFACE
// index, and a building material is never in that path at all. So the renderer
// classified from surface channels alone and could reach only glass and metal
// (SurfaceClassifier), while concrete, wood, earth and plastic — the substances
// a building is mostly made of — stayed out of reach with the answer already
// computed one level away.
//
// ---------------------------------------------------------------------------
// ⚠️ THE JOIN IS MANY-TO-MANY, AND THAT IS THE WHOLE DIFFICULTY. It would be
// convenient if each surface belonged to one building material. It does not:
//
//   * one BODY has one building material, and one element has many bodies;
//   * one body's polygons can carry SEVERAL surfaces (a composite wall's core
//     shows a cut surface on its ends and a finish on its faces);
//   * one SURFACE is reused across the whole project — "Paint - White" sits on
//     a concrete wall, a timber stud partition and a plasterboard ceiling.
//
// The renderer's material ranges are keyed by surface, so the answer has to be
// per surface: what substance is the thing wearing this surface USUALLY made
// of, in this project?
//
// ⚠️ AND THE HONEST ANSWER IS OFTEN "NO ONE SUBSTANCE". That is why this is a
// weighted vote with a REFUSAL rather than a majority: a surface used on
// concrete two thirds of the time and on timber one third gets no substance at
// all, because giving it concrete would render every timber element wearing it
// wrongly and confidently. It is the same rule SurfaceClassifier and
// BuildingMaterialSignal already apply for the same measured reason — a
// misclassified surface is worse than an unclassified one.
//
// ⚠️ AN UNCLASSIFIABLE BODY VOTES TOO, in the denominator. A surface whose
// bodies are mostly building materials BuildingMaterialSignal refused to name
// has weak evidence by definition, and letting a confident minority carry it
// would invert the refusal that was just made one level down.
//
// ⚠️ PURE. No ACAPI, no ModelerAPI, no Diligent, no I/O — so the offline suite
// runs it over hand-built observation sets AND over the real dumped projects,
// exactly as BuildingMaterialSignal is tested.

#include "ArchViz/BuildingMaterialSignal.hpp"
#include "ArchViz/MaterialTable.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace geomsrv {
namespace archviz {

// One body's worth of evidence: this surface appeared on something made of
// this substance, over this much of the body.
struct SurfaceSubstanceObservation {
    // The MODEL POOL index, 1-based — the number `Mesh::triMaterial` carries.
    // ⚠️ NOT an Archicad attribute index; see MaterialTable.hpp.
    int32_t surface = 0;

    // What BuildingMaterialSignal said about the body's building material.
    // `Unknown` is a real, expected value and is counted, not skipped.
    Substance substance = Substance::Unknown;

    // How much of the model this observation speaks for. Polygon count is what
    // the producer uses; any monotone measure of size works, and the unit never
    // leaves this file. ⚠️ IT MUST NOT BE 1 PER BODY: a curtain wall mullion and
    // a whole floor slab would then carry equal weight, and the small parts of a
    // building outnumber the large ones by an order of magnitude.
    double weight = 0.0;
};

struct SurfaceSubstance {
    Substance substance = Substance::Unknown;
    // The winning substance's share of ALL weight observed for this surface,
    // 0..1. 0 when the surface is refused.
    float confidence = 0.0f;
};

// ⚠️ 0.90, THE SAME BAR BuildingMaterialSignal USES FOR AGREEMENT, and chosen
// for the same reason rather than by analogy: below it, the surface is being
// used on more than one substance often enough that the renderer would visibly
// misdraw the minority. It is generous about noise (a stray sliver body cannot
// veto a surface) and strict about genuine sharing.
constexpr float kSubstanceDominance = 0.90f;

// Fold the observations into one verdict per surface.
//
// Empty in -> empty out. A surface that appears only with `Unknown` bodies is
// present in the result with substance `Unknown` and confidence 0, NOT absent:
// "asked and refused" and "never seen" are different facts and the caller can
// tell them apart.
std::map<int32_t, SurfaceSubstance> JoinSurfaceSubstances (
    const std::vector<SurfaceSubstanceObservation>& observations,
    float dominance = kSubstanceDominance);

// ---- carrying a verdict from one extraction pass to the next ----------------
//
// ⚠️ THE VOTE IS A FULL-PASS COST AND MUST NOT BECOME A PER-EDIT ONE. Deriving
// it needs every element's bodies and every body's polygons -- affordable once
// per rebuild, and exactly what a partial refresh exists to avoid paying. So a
// full pass computes it and a partial pass carries it forward.
//
// ⚠️ KEYED BY INDEX *AND* NAME, BECAUSE THE INDEX ALONE IS NOT AN IDENTITY.
// Archicad RENUMBERS the model's surface pool on every 3D rebuild, so index 7
// after an edit is very often a different surface from index 7 before it.
// Carrying the memory across blindly paints one surface with another's
// substance -- a wall that suddenly renders like polished metal, attributable to
// nothing. The name catches the renumbering and the memory is dropped instead,
// which costs one dull frame until the next full pass and cannot be wrong.
//
// ⚠️ AND THAT IS THE STANDING NAME RULE OBSERVED, NOT BENT. The rule
// (SurfaceClassifier.hpp, BuildingMaterialSignal.hpp) forbids a name DECIDING
// what a surface is. It does not forbid asking whether two indices refer to the
// same surface, which is the only thing done with it here.
using SubstanceMemory = std::map<int32_t, std::pair<std::string, SurfaceSubstance>>;

// Copy remembered verdicts onto a freshly read table, index by index, skipping
// any whose name no longer matches.
void ApplySubstanceMemory (const SubstanceMemory& memory, MaterialTable& table);

// Fold `observations` into per-surface verdicts, write them onto a copy of
// `surfaces`, refresh `memory`, and return the table to publish. `named` is set
// to how many surfaces came out with a substance -- the coverage number the log
// and the probe report.
std::unique_ptr<MaterialTable> BuildSubstanceTable (
    const std::vector<SurfaceMaterial>& surfaces,
    const std::vector<SurfaceSubstanceObservation>& observations,
    SubstanceMemory& memory, int& named);

} // namespace archviz
} // namespace geomsrv

#endif
