// ArchViz/ExtractionSubstance — see the header for why the join is many-to-many
// and why the element index is the one thing here that a live run has to settle.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/ExtractionSubstance.hpp"

#include <AttributeIndex.hpp>
#include <Model.hpp>
#include <ModelElement.hpp>
#include <ModelMeshBody.hpp>
#include <Polygon.hpp>

#include <map>

namespace geomsrv {
namespace archviz {

namespace {

// An API_AttributeIndex as the opaque per-project token the maps are keyed by.
//
// ⚠️ IT IS NOT THE GUID BuildingMaterialDump RECORDED, and it does not need to
// be. Both sides of every comparison in this file come from the SAME API in the
// SAME session, and BuildingMaterialSignal treats the fill key as opaque by
// design (BuildingMaterialRow::cutFillId, "a per-project attribute guid, opaque
// here"). Using the index costs no lookup and cannot fail; using the guid would
// mean a second attribute read per material for a token with the same meaning.
std::string AttributeKey (const API_AttributeIndex& index)
{
    return index.ToUniString ().ToCStr ().Get ();
}

} // namespace

ProjectSubstances ReadProjectSubstances ()
{
    ProjectSubstances result;

    GS::Array<API_Attribute> attributes;
    const GSErrCode err = ACAPI_Attribute_GetAttributesByType (API_BuildingMaterialID, attributes);
    if (err != NoError) {
        result.error = "ACAPI_Attribute_GetAttributesByType(API_BuildingMaterialID) failed";
        return result;
    }

    // ⚠️ TWO PASSES, AND THE ORDER IS FORCED. The fill -> substance map is
    // DERIVED from this project's own stock-priority materials, so every row has
    // to be collected before any row can be classified. Classifying inside the
    // first loop would use a half-built fill map, which silently changes the
    // answer for whichever materials happen to be enumerated first.
    std::vector<BuildingMaterialRow> rows;
    std::vector<std::string> keys;
    rows.reserve (attributes.GetSize ());
    keys.reserve (attributes.GetSize ());

    for (const API_Attribute& attribute : attributes) {
        BuildingMaterialRow row;
        row.connectionPriority = static_cast<int> (attribute.buildingMaterial.connPriority);
        row.cutFillId = AttributeKey (attribute.buildingMaterial.cutFill);
        rows.push_back (row);
        keys.push_back (AttributeKey (attribute.header.index));
    }
    result.total = static_cast<int> (rows.size ());

    const std::map<std::string, Substance> fillSubstances = DeriveFillSubstances (rows);
    for (size_t i = 0; i < rows.size (); ++i) {
        const SubstanceVerdict verdict = ClassifyBuildingMaterial (rows[i], fillSubstances);
        // ⚠️ ONLY THE AGREEING TIER REACHES THE RENDERER. ClassifyBuildingMaterial
        // also returns a 0.55 tier where the priority is a known stock value and
        // the fill has no opinion; that tier is the one measured to call marble
        // wood, and its own header says it is to be reported and not acted on.
        // Acting on it here would undo the measurement that produced the number.
        if (verdict.substance == Substance::Unknown || verdict.confidence < 0.90f)
            continue;
        result.byAttribute[keys[i]] = verdict.substance;
        ++result.classified;
    }
    return result;
}

void ObserveElementSubstances (const ModelerAPI::Model& model, int32_t index1Based,
                               const ProjectSubstances& substances,
                               std::vector<SurfaceSubstanceObservation>& out)
{
    if (index1Based < 1)
        return;

    ModelerAPI::Element element;
    model.GetElement (index1Based, &element);

    const Int32 bodyCount = element.GetTessellatedBodyCount ();
    for (Int32 iBody = 1; iBody <= bodyCount; ++iBody) {
        // ⚠️ BOTH INDICES DROP TO 0-BASED HERE, AND ONLY HERE. ModelerAPI counts
        // from 1 and the ModelAccess C API counts from 0; mixing the two is the
        // single most likely way for this file to be quietly wrong, so the
        // conversion happens once, at the call, rather than in the loop bounds.
        API_AttributeIndex materialIndex;
        const GSErrCode err = ACAPI_ModelAccess_GetBuildingMaterial (
            static_cast<UInt32> (index1Based - 1), static_cast<UInt32> (iBody - 1), &materialIndex);

        Substance substance = Substance::Unknown;
        if (err == NoError) {
            const auto found = substances.byAttribute.find (AttributeKey (materialIndex));
            if (found != substances.byAttribute.end ())
                substance = found->second;
        }

        // Which surfaces this body wears, and over how many polygons each.
        //
        // ⚠️ POLYGONS, NOT TRIANGLES, AND NOT VERTICES. The triangle count would
        // need the convex decomposition GeometryExtractor already pays for and
        // this walk deliberately does not repeat; polygon count is monotone in
        // it and is available from the loop bound itself. The weight's unit
        // never leaves SubstanceJoin, so any monotone measure is admissible.
        ModelerAPI::MeshBody body;
        element.GetTessellatedBody (iBody, &body);
        const Int32 polygonCount = body.GetPolygonCount ();

        std::map<int32_t, double> perSurface;
        for (Int32 iPoly = 1; iPoly <= polygonCount; ++iPoly) {
            ModelerAPI::Polygon polygon;
            body.GetPolygon (iPoly, &polygon);
            ModelerAPI::AttributeIndex surfaceIndex;
            polygon.GetMaterialIndex (surfaceIndex);
            perSurface[static_cast<int32_t> (surfaceIndex.GetIndex ())] += 1.0;
        }

        for (const auto& entry : perSurface) {
            SurfaceSubstanceObservation observation;
            observation.surface = entry.first;
            observation.substance = substance;
            observation.weight = entry.second;
            out.push_back (observation);
        }
    }
}

} // namespace archviz
} // namespace geomsrv
