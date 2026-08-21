// ArchViz/BuildingMaterialSignal — what a surface IS, from the building
// material behind the element rather than from the surface in front of it.
//
// ⚠️ WHY THIS EXISTS, AND WHY SurfaceClassifier CANNOT DO IT. RE51.B1 decides a
// surface's type from its numeric channels and succeeds for exactly two classes,
// glass and metal, because transparency and specular reflection are genuinely
// optical properties. It cannot reach earth, concrete, plastic or wood, and the
// reason is structural rather than a missing threshold. Measured on the real
// template (dumps/surface_template__20260821_115759):
//
//     Wood - Pine Grained Horizontal   shine 0  specular 19  diffuse 62
//     Plytos - TAMSIAI RUDOS           shine 0  specular 19  diffuse 62
//
// Wood and brick, identical on every channel an Archicad SURFACE exposes.
// Colour does not separate them either -- mahogany sits at hue 12 / saturation
// 0.48 and aged brick at hue 13 / 0.37. An Archicad surface describes how
// something LOOKS. What it IS lives one level down, on the BUILDING MATERIAL,
// and a structural concrete is concrete whatever its surface is called.
//
// ⚠️ AND THE BREAKDOWN'S PREMISE FOR THIS UNIT IS WRONG. RE51.B2 says building
// materials carry "physical properties". They carry none: no density, no
// conductivity, no substance type. The official API's `BuildingMaterialAttribute`
// (archicad package, releases/ac29/b3000types.py) is exactly seven fields, and
// only two of them are usable here -- `connectionPriority` and `cutFillId`.
// Measured with Diagnostics/Commands/BuildingMaterialDump on two unrelated
// projects, 2026-08-21.
//
// ⚠️ NAMES MUST NEVER DECIDE. The same standing constraint as SurfaceClassifier,
// and this file is where it would have been easiest to break: every building
// material has a perfectly descriptive English or Lithuanian name sitting right
// there. Neither input below is a name. The test file reads names only to CHECK
// the rules, which is the one legitimate use.
//
// ---------------------------------------------------------------------------
// SIGNAL 1 -- connectionPriority. Archicad's intersection precedence: which
// material wins where two meet. It is an INT, it is authored per material, and
// it turns out to be a STANDARD SCALE: of the 59 material names present in both
// measured projects, 58 carry an IDENTICAL priority. The one exception is
// "Tile - Floor" (230 in one project, 542 in the other), which proves the number
// is user-editable and is exactly why it cannot be trusted alone.
//
// ⚠️ IT IS NOT A SUBSTANCE ORDINAL. The same substance appears at many
// priorities according to its ROLE -- timber is 910 structural, 810 roof and 220
// floor -- so no band or threshold can work. It is a LOOKUP of the stock values,
// nothing more.
//
// SIGNAL 2 -- cutFillId. The cut fill is the hatch drawn where the material is
// sectioned, and materials of one substance share one. Measured, it groups the
// substance the priority scatters: the three timbers at 220/810/910 carry ONE
// fill, the three bricks at 540/640/720 carry one, the three concretes at
// 740/760/920 carry one.
//
// ⚠️ THE FILL GUID IS PER-PROJECT AND CANNOT BE TABULATED. The two measured
// projects share no fill id at all. So the map from fill to substance is DERIVED
// per project from the stock priorities, and only then used to classify the
// materials the priority table does not know -- which is how a user-authored
// material gets classified without anybody reading its name.
//
// ---------------------------------------------------------------------------
// ⚠️ NEITHER SIGNAL IS USED ALONE, AND THAT IS THE WHOLE DESIGN. Measured, each
// on its own is confidently wrong somewhere:
//
//   priority alone  calls "## CONCEPT - MARBLE" WOOD, because the user authored
//                   it at 350, which is Plywood's slot.
//   fill alone      calls "GENERIC - ENVIRONMENT" GLASS, because glass shares
//                   the template's default fill with every GENERIC material;
//                   and calls gypsum plasterboard WOOD.
//
// Requiring the two to AGREE removed every one of those errors. Across both
// projects and 157 building materials the agreeing verdict is correct 100% of
// the time, including three user-authored materials whose names were never read.
// The price is coverage, and it is paid deliberately: a misclassified surface is
// worse than an unclassified one, because it is confidently wrong.

#pragma once

#include <map>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {

// The six substances asked for, plus the refusal. Deliberately NOT a complete
// taxonomy of construction: brick, stone, plaster, gypsum, insulation and
// membrane are all present in the measured projects and all deliberately absent
// here, because the renderer has nothing different to do with them yet. Adding
// one means giving it a preset, not just a name.
enum class Substance {
    Unknown = 0,
    Earth,
    Concrete,
    Metal,
    Plastic,
    Glass,
    Wood,
};

struct SubstanceVerdict {
    Substance substance = Substance::Unknown;
    // 0.90 when both signals agree -- the only tier a renderer should act on.
    // 0.55 when the priority is a known stock value and the fill has NO opinion.
    //      ⚠️ REPORTED, NOT TO BE ACTED ON: this is the tier that calls marble
    //      wood. It exists so a caller can SEE the weaker signal rather than
    //      have it silently folded into the strong one.
    // 0.00 when unclassified.
    float confidence = 0.0f;
};

// One row of what BuildingMaterialDump reads. ⚠️ NO NAME FIELD, ON PURPOSE:
// this struct is the classifier's whole input, so a name cannot leak into a
// decision even by accident.
struct BuildingMaterialRow {
    int connectionPriority = -1;
    std::string cutFillId; // a per-project attribute guid, opaque here
};

// Archicad's stock connectionPriority values, measured identical across two
// unrelated projects. Exposed so the test can assert the table rather than
// re-derive it, and so a future measurement can extend it in one place.
const std::map<int, Substance>& StockPriorities ();

// Derive this PROJECT's fill -> substance map from the materials whose priority
// is a known stock value.
//
// ⚠️ A FILL NEEDS TWO INDEPENDENT WITNESSES. Trusting a fill whose only
// stock-priority member is the material being classified is circular, and
// measured it is what called gypsum plasterboard wood: the board sat at
// Fiberboard's priority and was the sole known member of its own fill, so the
// fill "confirmed" the very thing it was derived from. Two agreeing members, or
// the fill says nothing.
std::map<std::string, Substance> DeriveFillSubstances (const std::vector<BuildingMaterialRow>& rows);

// ⚠️ PURE. No ACAPI, no Diligent, no I/O -- so the offline suite runs it over
// the real dumps of two real projects instead of over invented materials.
SubstanceVerdict ClassifyBuildingMaterial (const BuildingMaterialRow& row,
                                           const std::map<std::string, Substance>& fillSubstances);

const char* SubstanceName (Substance substance);

} // namespace archviz
} // namespace geomsrv
