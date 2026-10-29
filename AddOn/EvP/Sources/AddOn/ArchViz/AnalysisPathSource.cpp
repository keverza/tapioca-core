// ArchViz/AnalysisPathSource -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/AnalysisPathSource.hpp"
#include "ArchViz/SelectionMetadata.hpp"
#include "ArchViz/SlabSliceSource.hpp"

#include "Diagnostics/ApiError.hpp" // DescribeErr - never print a bare GSErrCode
#include "Metadata/MetadataStorage.hpp"
#include "NativeCommands/CommandUtils.hpp" // WalkPolygonRings: the one walk of a polygon memo

#include <atomic>
#include <mutex>
#include <vector>

namespace geomsrv::archviz::analysispathsource {

namespace {

namespace meta = metadata;

std::mutex s_mutex;
Imported s_latest;
Saved s_saved;
std::atomic<bool> s_refreshing { false };

std::string Describe (GSErrCode err)
{
    return evp::DescribeErr (err).ToCStr (0, MaxUSize, CC_UTF8).Get ();
}

double StoreyLevel (short index)
{
    API_StoryInfo info {};
    double level = 0.0;
    if (ACAPI_ProjectSetting_GetStorySettings (&info) != NoError || info.data == nullptr)
        return level;
    for (short i = 0; i <= info.lastStory - info.firstStory; ++i)
        if ((*info.data)[i].index == index)
            level = (*info.data)[i].level;
    BMKillHandle (reinterpret_cast<GSHandle*> (&info.data));
    return level;
}

// The polyline's one contour, or false with `error`.
bool ReadPolyline (const API_Element& element, analysispath::Path& path, std::string& error)
{
    API_ElementMemo memo {};
    const GSErrCode err = ACAPI_Element_GetMemo (element.header.guid, &memo, APIMemoMask_Polygon);
    if (err != NoError) {
        ACAPI_DisposeElemMemoHdls (&memo);
        error = "the Polyline could not be read (ACAPI_Element_GetMemo: " + Describe (err) + ")";
        return false;
    }
    PolygonHandles polygon;
    polygon.coords = memo.coords;
    polygon.pends = memo.pends;
    polygon.parcs = memo.parcs;
    GS::Array<double> outer, outerArcs, holes, holeArcs;
    GS::Array<GS::Int32> holeCounts;
    GS::Int32 outerCount = 0, holeTotal = 0;
    bool closed = false;
    // Polyline mode: an open polyline does not repeat its first node.
    const bool walked = WalkPolygonRings (polygon, nullptr, outer, outerArcs, outerCount, holes, holeArcs, holeCounts,
                                          holeTotal, true, &closed);
    ACAPI_DisposeElemMemoHdls (&memo);
    if (!walked || outerCount < 2) {
        error = "the Polyline has fewer than two points";
        return false;
    }
    std::vector<double> xy, arcs;
    for (GS::Int32 i = 0; i < outerCount; ++i) {
        xy.push_back (outer[UIndex (i * 2)]);
        xy.push_back (outer[UIndex (i * 2 + 1)]);
        arcs.push_back (outerArcs[UIndex (i)]);
    }
    path = analysispath::Build (APIGuidToString (element.header.guid).ToCStr ().Get (), xy, arcs, closed,
                                StoreyLevel (element.header.floorInd));
    if (path.points.empty ()) {
        error = "the Polyline has no length";
        return false;
    }
    path.stamp = uint64_t (element.header.modiStamp);
    return true;
}

// The profile kept on the Polyline, if any. MAIN THREAD.
void ReadProfile (const std::string& guid, Imported& imported)
{
    meta::EntityMetadata entity;
    bool present = false;
    std::string error;
    if (!meta::storage::Read (guid, entity, present, error)) {
        imported.error = "its height profile could not be read: " + error;
        return;
    }
    const meta::Property* property = present ? meta::FindProperty (entity, analysisprofile::kPropertyKey) : nullptr;
    if (property == nullptr)
        return;
    auto profile = std::make_shared<analysisprofile::Profile> ();
    if (!analysisprofile::Decode (*property, *profile, error)) {
        imported.error = "its height profile was not used: " + error;
        return;
    }
    imported.profile = std::move (profile);
}

// MAIN THREAD.
bool Save (const std::string& guid, const analysisprofile::Profile& profile, std::string& error)
{
    meta::EntityMetadata entity;
    bool present = false;
    if (!meta::storage::Read (guid, entity, present, error))
        return false;
    meta::Property property = analysisprofile::Encode (profile);
    const meta::Property* old = meta::FindProperty (entity, analysisprofile::kPropertyKey);
    if (old != nullptr && old->value == property.value)
        return true; // unchanged: no undo step
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
    const GSErrCode result = ACAPI_CallUndoableCommand ("Tapioca analysis path profile", [&] () -> GSErrCode {
        return meta::storage::Write (guid, entity, error) ? NoError : APIERR_GENERAL;
    });
    if (result != NoError && error.empty ())
        error = "the profile was not stored (" + Describe (result) + ")";
    return result == NoError;
}

void Publish (Imported imported)
{
    std::lock_guard<std::mutex> lock (s_mutex);
    imported.serial = s_latest.serial + 1;
    s_latest = std::move (imported);
}

// MAIN THREAD.
void Import ()
{
    Imported imported;
    std::vector<std::string> guids;
    std::string error;
    if (!slabsource::Selected (guids, error))
        imported.error = error;
    for (const auto& guid : guids) {
        API_Element element {};
        element.header.guid = APIGuidFromString (guid.c_str ());
        if (ACAPI_Element_Get (&element) != NoError || element.header.type.typeID != API_PolyLineID)
            continue;
        auto path = std::make_shared<analysispath::Path> ();
        if (ReadPolyline (element, *path, imported.error)) {
            imported.path = std::move (path);
            ReadProfile (guid, imported);
        }
        break;
    }
    if (imported.path == nullptr && imported.error.empty ())
        imported.error = "select a Polyline in Archicad first";
    Publish (std::move (imported));
}

// MAIN THREAD.
void Refresh (const std::string& guid, uint64_t stamp)
{
    API_Element element {};
    element.header.guid = APIGuidFromString (guid.c_str ());
    API_Elem_Head head {};
    head.guid = element.header.guid;
    if (ACAPI_Element_GetHeader (&head) != NoError) {
        Imported gone;
        gone.refreshed = gone.gone = true;
        gone.error = "the Polyline is no longer in the project";
        Publish (std::move (gone));
        return;
    }
    if (uint64_t (head.modiStamp) == stamp)
        return;
    Imported imported;
    imported.refreshed = true;
    auto path = std::make_shared<analysispath::Path> ();
    if (ACAPI_Element_Get (&element) == NoError && ReadPolyline (element, *path, imported.error))
        imported.path = std::move (path);
    else if (imported.error.empty ())
        imported.error = "the edited Polyline could not be read";
    Publish (std::move (imported));
}

} // namespace

void RequestImport ()
{
    if (selectionmetadata::Later ([] { Import (); }))
        return;
    std::lock_guard<std::mutex> lock (s_mutex);
    ++s_latest.serial;
    s_latest.path.reset ();
    s_latest.error = "the import was not queued: the project message loop is unavailable";
}

Imported Latest ()
{
    std::lock_guard<std::mutex> lock (s_mutex);
    return s_latest;
}

void RequestRefresh (std::string guid, uint64_t stamp)
{
    if (s_refreshing.exchange (true))
        return;
    if (!selectionmetadata::Later ([guid, stamp] () {
            Refresh (guid, stamp);
            s_refreshing.store (false);
        }))
        s_refreshing.store (false);
}

void RequestSave (std::string guid, analysisprofile::Profile profile)
{
    const auto finish = [] (std::string guid, std::string error) {
        std::lock_guard<std::mutex> lock (s_mutex);
        s_saved = { s_saved.serial + 1, std::move (guid), std::move (error) };
    };
    if (selectionmetadata::Later ([guid, profile = std::move (profile), finish] () {
            std::string error;
            Save (guid, profile, error);
            finish (guid, error);
        }))
        return;
    finish (std::move (guid), "the save was not queued: the project message loop is unavailable");
}

Saved LastSave ()
{
    std::lock_guard<std::mutex> lock (s_mutex);
    return s_saved;
}

} // namespace geomsrv::archviz::analysispathsource
