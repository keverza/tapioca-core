#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/MassingModel.hpp"
#include "ArchViz/MassingRulesModel.hpp"
#include "ArchViz/MassingHybrid.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/HudConsole.hpp"
#include "ArchViz/OverlayHudModel.hpp"
#include "ArchViz/SelectionMetadata.hpp"
#include "Diagnostics/ApiError.hpp"
#include "Metadata/MetadataStorage.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace geomsrv::archviz::massingmodel {
namespace {
namespace meta = metadata;
hudmassing::Page s_page;
bool s_dirty = true;
std::string s_notice;
uint64_t s_projectEpoch = 0;

std::string Describe (GSErrCode code)
{
    return evp::DescribeErr (code).ToCStr (0, MaxUSize, CC_UTF8).Get ();
}

// Borrow the model database, with restoration on every exit (including failure).
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

bool Scan (hudmassing::Page& page, std::set<std::string>* modelGuids = nullptr)
{
    ModelDatabase database;
    if (database.error != NoError) {
        page.note = "Cannot read the massing model database: " + Describe (database.error);
        return false;
    }
    for (const API_ElemTypeID type : { API_PolyLineID, API_MeshID, API_SlabID }) {
        GS::Array<API_Guid> guids;
        const GSErrCode result = ACAPI_Element_GetElemList (type, &guids);
        if (result != NoError) {
            page.note = "Cannot list massing elements: " + Describe (result);
            return false;
        }
        for (const API_Guid& guid : guids) {
            meta::EntityMetadata entity;
            bool present = false;
            std::string error;
            const std::string id = APIGuidToString (guid).ToCStr ().Get ();
            if (modelGuids != nullptr)
                modelGuids->insert (id);
            if (!meta::storage::Read (id, entity, present, error)) {
                page.note = error;
                return false; // Never replace/clear an incompletely read group.
            }
            if (type == API_SlabID)
                page.buildingSlabs.push_back ({ id, massingbuildings::Id (entity) });
            const meta::Property* role = meta::FindProperty (entity, "tapioca.role");
            if (role == nullptr)
                continue;
            for (size_t i = 0; i < page.guids.size (); ++i)
                if (role->value.s == hudmassing::Role (hudmassing::Group (i)))
                    page.guids[i].push_back (id);
        }
    }
    page.known = true;
    return true;
}

bool Compatible (hudmassing::Group group, const std::string& guid, std::string& error)
{
    API_Element element {};
    element.header.guid = APIGuidFromString (guid.c_str ());
    const GSErrCode result = ACAPI_Element_Get (&element);
    if (result != NoError) {
        error = "Cannot read selected element: " + Describe (result);
        return false;
    }
    const API_ElemTypeID type = element.header.type.typeID;
    bool closed = false;
    if (type == API_PolyLineID && element.polyLine.poly.nSubPolys == 1 && element.polyLine.poly.nCoords >= 4) {
        API_ElementMemo memo {};
        const GSErrCode read = ACAPI_Element_GetMemo (element.header.guid, &memo, APIMemoMask_Polygon);
        const Int32 count = element.polyLine.poly.nCoords;
        if (read == NoError && memo.coords != nullptr &&
            BMGetHandleSize (reinterpret_cast<GSHandle> (memo.coords)) >= (count + 1) * sizeof (API_Coord)) {
            const auto a = (*memo.coords)[1], b = (*memo.coords)[count];
            closed = std::hypot (a.x - b.x, a.y - b.y) <= 1e-7;
        }
        ACAPI_DisposeElemMemoHdls (&memo);
    }
    const bool valid = group == hudmassing::Group::PropertyLine   ? type == API_PolyLineID && closed
                       : group == hudmassing::Group::MassingSlabs ? type == API_SlabID
                                                                  : type == API_MeshID;
    if (!valid)
        error = std::string (hudmassing::Label (group)) + " requires " +
                (group == hudmassing::Group::PropertyLine   ? "a closed Polyline"
                 : group == hudmassing::Group::MassingSlabs ? "Slabs"
                                                            : "terrain Meshes");
    return valid;
}

void Apply (hudmassing::Request request, const std::vector<std::string>& selected)
{
    hudmassing::Page page;
    std::string error;
    std::set<std::string> modelGuids;
    if (!Scan (page, &modelGuids)) {
        error = page.note;
    }
    else {
        const auto& current = page.guids[size_t (request.group)];
        if (request.action == hudmassing::Action::Reselect) {
            GS::Array<API_Neig> neigs;
            for (const auto& guid : current)
                neigs.Push (API_Neig (APIGuidFromString (guid.c_str ())));
            GSErrCode result = ACAPI_Selection_DeselectAll ();
            if (result == APIERR_NOSEL)
                result = NoError;
            if (result == NoError && !neigs.IsEmpty ())
                result = ACAPI_Selection_Select (neigs, true);
            if (result != NoError)
                error = "Cannot reselect massing elements: " + Describe (result);
        }
        else {
            const bool adding =
                request.action == hudmassing::Action::Add || request.action == hudmassing::Action::Update;
            if (adding && selected.empty ())
                error = "Select elements in the viewport first; use Clear to empty a group.";
            if (adding && error.empty ())
                for (const auto& guid : selected) {
                    if (!modelGuids.count (guid)) {
                        error = "Define uses model elements, not worksheet/detail elements.";
                        break;
                    }
                    if (!Compatible (request.group, guid, error))
                        break;
                }
            std::set<std::string> future (selected.begin (), selected.end ());
            if (request.action == hudmassing::Action::Add)
                future.insert (current.begin (), current.end ());
            if (adding && request.group == hudmassing::Group::PropertyLine &&
                future.size () > massingcalculation::kMaxParcels)
                error = "Define accepts at most 32 property-line Polylines.";
            const auto plan = hudmassing::Plan (request, current, selected);
            // Read and validate EVERY record before opening the single undo step.
            std::vector<std::pair<std::string, meta::EntityMetadata>> writes;
            meta::ProjectSchema schema;
            bool stored = false;
            if (error.empty ())
                meta::storage::ReadSchema (schema, stored, error);
            if (error.empty ())
                for (const auto& assignment : plan) {
                    meta::EntityMetadata entity;
                    bool present = false;
                    if (!meta::storage::Read (assignment.guid, entity, present, error))
                        break;
                    const auto* role = meta::FindProperty (entity, "tapioca.role");
                    if (!assignment.assign && (role == nullptr || role->value.s != hudmassing::Role (request.group)))
                        continue;
                    entity.properties.erase (
                        std::remove_if (entity.properties.begin (), entity.properties.end (),
                                        [] (const meta::Property& p) { return p.key == "tapioca.role"; }),
                        entity.properties.end ());
                    if (assignment.assign) {
                        meta::Property property;
                        property.key = "tapioca.role";
                        property.value = meta::Value::Text (hudmassing::Role (request.group));
                        entity.properties.push_back (std::move (property));
                    }
                    const auto issues = meta::Validate (entity, schema);
                    if (!issues.empty ()) {
                        error = issues.front ();
                        break;
                    }
                    writes.emplace_back (assignment.guid, std::move (entity));
                }
            if (error.empty () && !writes.empty ()) {
                const GSErrCode result = ACAPI_CallUndoableCommand ("Tapioca Massing Define", [&] () -> GSErrCode {
                    for (const auto& write : writes)
                        if (!meta::storage::Write (write.first, write.second, error))
                            return APIERR_GENERAL; // Archicad rolls back this undo command.
                    return NoError;
                });
                if (result != NoError && error.empty ())
                    error = Describe (result);
            }
        }
    }
    s_dirty = true;
    s_notice = error;
    ArchVizLog (std::string ("MASSING DEFINE  ") + hudmassing::Label (request.group) + " action=" +
                std::to_string (int (request.action)) + (error.empty () ? " completed" : " refused: " + error));
    if (!error.empty ())
        hudconsole::Say (hudconsole::Level::Error, "Massing Define", error);
    selectionmetadata::Changed ();
    overlayhudmodel::SelectionChanged ();
}
} // namespace

hudmassing::Page Read ()
{
    if (s_dirty) {
        s_dirty = false;
        hudmassing::Page page;
        Scan (page);
        if (page.known) {
            std::sort (page.guids[0].begin (), page.guids[0].end ());
            if (page.guids[0].size () > massingcalculation::kMaxParcels)
                page.note = "At most 32 property lines are supported; use Define to remove excess roles.";
            else
                page.parcels = massingrulesmodel::ReadParcels (page.guids[0]);
            if (!page.parcels.empty ())
                page.rules = page.parcels.front ();
        }
        if (page.note.empty ())
            page.note = s_notice;
        s_page = std::move (page);
    }
    return s_page;
}

void Request (hudmassing::Request request)
{
    // Capture selection before posting, so a later viewport click cannot retarget it.
    const auto guids = selectionmetadata::SelectedGuids ();
    const uint64_t epoch = s_projectEpoch;
    if (!selectionmetadata::Later ([request, guids, epoch] () {
            if (epoch == s_projectEpoch)
                Apply (request, guids);
        })) {
        s_notice = "Massing edit was not queued: the project message loop is unavailable.";
        s_dirty = true;
        hudconsole::Say (hudconsole::Level::Error, "Massing Define", s_notice);
    }
}

void RequestRules (massingrules::Edit edit)
{
    const uint64_t epoch = s_projectEpoch;
    if (!selectionmetadata::Later ([edit = std::move (edit), epoch] () {
            if (epoch != s_projectEpoch)
                return;
            std::string error;
            hudmassing::Page current;
            if (!Scan (current))
                error = current.note;
            else if (std::find (current.guids[0].begin (), current.guids[0].end (), edit.before.guid) ==
                     current.guids[0].end ())
                error = "Defined property line changed before Save; reread before editing.";
            else
                massingrulesmodel::Apply (edit, error);
            s_notice = error;
            s_dirty = true;
            ArchVizLog ("MASSING RULES  " + edit.before.guid + (error.empty () ? " saved" : " refused: " + error));
            if (!error.empty ())
                hudconsole::Say (hudconsole::Level::Error, "Property line rules", error);
            selectionmetadata::Changed ();
            overlayhudmodel::SelectionChanged ();
        })) {
        s_notice = "Property-line save was not queued: the project message loop is unavailable.";
        s_dirty = true;
        hudconsole::Say (hudconsole::Level::Error, "Property line rules", s_notice);
    }
}

void Changed ()
{
    s_dirty = true;
}
void Forget ()
{
    massinghybrid::Forget ();
    ++s_projectEpoch;
    s_page = {};
    s_notice.clear ();
    s_dirty = true;
}
} // namespace geomsrv::archviz::massingmodel
