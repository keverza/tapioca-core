#ifndef EVP_ARCHVIZ_EXTRACTIONSUBSTANCE_HPP
#define EVP_ARCHVIZ_EXTRACTIONSUBSTANCE_HPP

// ArchViz/ExtractionSubstance — the Archicad half of the substance join.
//
// It reads two things the geometry extraction never looked at, and it is the
// piece that finally gives ArchViz/BuildingMaterialSignal a caller:
//
//   1. the project's building materials, classified into substances;
//   2. for one model element, WHICH surfaces sit on WHICH of its bodies.
//
// ArchViz/SubstanceJoin then folds (2) through (1) into one verdict per
// surface, which is what MaterialTable carries and the shader consumes.
//
// ⚠️ MAIN THREAD ONLY, BOTH FUNCTIONS. One is ACAPI and the other is
// ModelerAPI; the extraction WORKER calls neither directly, it posts them
// through MainThreadGate exactly as it does ExtractElementAt. This is the same
// rule and the same seam as ExtractionEnvironment.
//
// ---------------------------------------------------------------------------
// ⚠️ THE INDEX JOIN IS AN ASSUMPTION UNTIL A LIVE RUN MEASURES IT, AND IT IS
// STATED HERE RATHER THAN BURIED. `ACAPI_ModelAccess_GetBuildingMaterial` takes
// a 0-BASED element index into the CURRENT MODEL SIGHT, plus a 0-based body
// index. The viewer walks the 3D window's sight with 1-BASED ModelerAPI
// indices, so the mapping used below is `elemIdx = index1Based - 1` -- which is
// exactly what NativeCommands/CuttingPlaneCommands.cpp does for its own sight.
//
// The two calls agreeing is not something a compiler or an offline test can
// check: a wrong index returns a DIFFERENT ELEMENT'S building material, with no
// error, and the result is a plausible-looking substance on the wrong surfaces.
// So `ObserveElementSubstances` also reports the element's own GUID beside what
// it read, and DiligentMaterialProbe asks Archicad the same question through
// Tapioca.GetBodyBuildingMaterials and compares. Do not treat the join as
// settled from reading this comment.

#include "ArchViz/BuildingMaterialSignal.hpp"
#include "ArchViz/SubstanceJoin.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ModelerAPI {
class Model;
}

namespace geomsrv {
namespace archviz {

// Every building material in the project, already run through
// BuildingMaterialSignal. Read once per extraction pass, like the surface pool.
struct ProjectSubstances {
    // Attribute-index string -> what it is made of. ⚠️ THE KEY IS AN OPAQUE
    // PER-PROJECT TOKEN and nothing may parse it; it is only ever compared with
    // what ACAPI_ModelAccess_GetBuildingMaterial hands back for a body.
    std::map<std::string, Substance> byAttribute;

    // What the read saw, for the probe and the log. ⚠️ COVERAGE IS THE NUMBER
    // THAT MATTERS: BuildingMaterialSignal deliberately refuses whenever its two
    // signals disagree, so a project where `classified` is far below `total` is
    // reporting a real property of that project's attributes, not a failure.
    int total = 0;
    int classified = 0;

    // Empty when the read succeeded. A non-empty reason leaves `byAttribute`
    // empty, which the join reads as "no substance anywhere" -- the renderer
    // then behaves exactly as it did before this file existed.
    std::string error;
};

// MAIN THREAD. Reads every API_BuildingMaterialID attribute, keeps the two
// fields that carry information without carrying language (connPriority and
// cutFill), derives this project's fill map, and classifies.
//
// ⚠️ NO NAME IS EVER READ. The standing constraint, and this is the function
// where breaking it would be easiest and most tempting -- every building
// material has a perfectly descriptive name sitting right there in the same
// struct. See BuildingMaterialSignal.hpp for the two independent reasons.
ProjectSubstances ReadProjectSubstances ();

// MAIN THREAD. For one element of `model`, by its 1-BASED ModelerAPI index,
// append one observation per (body, surface) pair to `out`.
//
// The weight is the body's polygon count for that surface, which is a proxy for
// area that costs nothing extra: the polygons are already being walked to find
// which surfaces the body uses. ⚠️ NOT ONE OBSERVATION PER BODY -- see
// SubstanceJoin.hpp on why equal weights let mullions outvote slabs.
//
// A body whose building material cannot be read contributes an `Unknown`
// observation rather than none, so it counts in the denominator of the vote.
void ObserveElementSubstances (const ModelerAPI::Model& model, int32_t index1Based,
                               const ProjectSubstances& substances,
                               std::vector<SurfaceSubstanceObservation>& out);

} // namespace archviz
} // namespace geomsrv

#endif
