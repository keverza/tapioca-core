#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/MassingRulesModel.hpp"
#include "Metadata/MetadataStorage.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::massingrulesmodel {
namespace {
namespace rules = massingrules;
namespace meta = metadata;

struct ModelDatabase {
    API_DatabaseInfo original {};
    bool changed = false;
    GSErrCode error = NoError;
    ModelDatabase ()
    {
        error = ACAPI_Database_GetCurrentDatabase (&original);
        if (error == NoError && original.typeID != APIWind_FloorPlanID) {
            API_DatabaseInfo model {};
            model.typeID = APIWind_FloorPlanID;
            error = ACAPI_Database_ChangeCurrentDatabase (&model);
            changed = error == NoError;
        }
    }
    ~ModelDatabase ()
    {
        if (changed)
            ACAPI_Database_ChangeCurrentDatabase (&original);
    }
};

bool Edges (const std::string& guid, std::vector<rules::Edge>& edges, std::string& error)
{
    const API_Guid id = APIGuidFromString (guid.c_str ());
    GS::Array<API_Guid> modelGuids;
    if (ACAPI_Element_GetElemList (API_PolyLineID, &modelGuids) != NoError ||
        std::find (modelGuids.Begin (), modelGuids.End (), id) == modelGuids.End ()) {
        error = "Property line is not a Polyline in the model database.";
        return false;
    }
    API_Element element {};
    element.header.guid = id;
    if (ACAPI_Element_Get (&element) != NoError || element.header.type.typeID != API_PolyLineID ||
        element.polyLine.poly.nSubPolys != 1 || element.polyLine.poly.nCoords < 4 ||
        element.polyLine.poly.nCoords > 257) {
        error = "Property line requires one closed 3..256 edge model Polyline.";
        return false;
    }
    API_ElementMemo memo {};
    struct Dispose {
        API_ElementMemo& memo;
        ~Dispose ()
        {
            ACAPI_DisposeElemMemoHdls (&memo);
        }
    } dispose { memo };
    const Int32 count = element.polyLine.poly.nCoords;
    const Int32 arcCount = element.polyLine.poly.nArcs;
    if (ACAPI_Element_GetMemo (id, &memo, APIMemoMask_Polygon) != NoError || memo.coords == nullptr ||
        BMGetHandleSize (reinterpret_cast<GSHandle> (memo.coords)) < (count + 1) * sizeof (API_Coord) || arcCount < 0 ||
        arcCount > count ||
        (arcCount > 0 && (memo.parcs == nullptr || BMGetHandleSize (reinterpret_cast<GSHandle> (memo.parcs)) <
                                                       arcCount * sizeof (API_PolyArc)))) {
        error = "Cannot read the property-line polygon memo.";
        return false;
    }
    const API_Coord first = (*memo.coords)[1], last = (*memo.coords)[count];
    if (std::hypot (first.x - last.x, first.y - last.y) > 1e-7) {
        error = "Property-line Polyline is open.";
        return false;
    }
    for (Int32 i = 1; i < count; ++i) {
        const API_Coord a = (*memo.coords)[i], b = (*memo.coords)[i + 1];
        edges.push_back ({ a.x, a.y, b.x, b.y, 0 });
    }
    for (Int32 i = 0; i < arcCount; ++i) {
        const auto& arc = (*memo.parcs)[i];
        if (arc.begIndex < 1 || arc.begIndex >= count || arc.endIndex != arc.begIndex + 1) {
            error = "Invalid property-line arc indices.";
            return false;
        }
        edges[size_t (arc.begIndex - 1)].arcAngle = arc.arcAngle;
    }
    if (!rules::ValidEdges (edges)) {
        error = "Property line contains invalid or collapsed edges.";
        return false;
    }
    return true;
}
} // namespace

rules::Page Read (const std::vector<std::string>& guids)
{
    rules::Page page;
    if (guids.empty ())
        return page;
    if (guids.size () != 1) {
        page.note = "Multiple property-line roles found. Use Define to replace them with one Polyline.";
        return page;
    }
    page.guid = guids.front ();
    ModelDatabase database;
    if (database.error != NoError) {
        page.note = "Cannot access the model database for property-line rules.";
        return page;
    }
    meta::EntityMetadata entity;
    bool present = false;
    if (Edges (page.guid, page.edges, page.note) && meta::storage::Read (page.guid, entity, present, page.note))
        rules::Restore (page, entity);
    return page;
}

std::vector<rules::Page> ReadParcels (const std::vector<std::string>& guids)
{
    std::vector<rules::Page> pages;
    for (const auto& guid : guids)
        pages.push_back (Read ({ guid }));
    return pages;
}

bool Apply (const rules::Edit& edit, std::string& error)
{
    ModelDatabase database;
    if (database.error != NoError) {
        error = "Cannot access the model database for property-line rules.";
        return false;
    }
    std::vector<rules::Edge> current;
    if (!edit.before.known || edit.before.guid.empty ()) {
        error = "No valid property-line snapshot was captured for Save.";
        return false;
    }
    if (!Edges (edit.before.guid, current, error))
        return false;
    meta::EntityMetadata entity;
    bool present = false;
    if (!meta::storage::Read (edit.before.guid, entity, present, error))
        return false;
    if (!rules::CheckSource (edit, current, entity, error))
        return false;
    const auto* old = meta::FindProperty (entity, "setback.segments");
    meta::Property property;
    if (!rules::Encode (current, edit.assignments, property, error))
        return false;
    if (old != nullptr && old->value == property.value)
        return true; // Unchanged Save does not consume an undo step.
    meta::SetProperty (entity, std::move (property));
    meta::ProjectSchema schema;
    bool stored = false;
    if (!meta::storage::ReadSchema (schema, stored, error))
        return false;
    const auto issues = meta::Validate (entity, schema);
    if (!issues.empty ()) {
        error = issues.front ();
        return false;
    }
    const GSErrCode result = ACAPI_CallUndoableCommand ("Tapioca Property line rules", [&] () -> GSErrCode {
        return meta::storage::Write (edit.before.guid, entity, error) ? NoError : APIERR_GENERAL;
    });
    if (result != NoError && error.empty ())
        error = "Property-line save failed (Archicad error " + std::to_string (result) + ").";
    return result == NoError;
}
} // namespace geomsrv::archviz::massingrulesmodel
