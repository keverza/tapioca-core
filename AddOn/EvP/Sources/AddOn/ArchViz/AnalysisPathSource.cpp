// ArchViz/AnalysisPathSource -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/AnalysisPathSource.hpp"
#include "ArchViz/SelectionMetadata.hpp"
#include "ArchViz/SlabSliceSource.hpp"

#include "Diagnostics/ApiError.hpp"        // DescribeErr - never print a bare GSErrCode
#include "NativeCommands/CommandUtils.hpp" // WalkPolygonRings: the one walk of a polygon memo

#include <mutex>
#include <vector>

namespace geomsrv::archviz::analysispathsource {

namespace {

std::mutex s_mutex;
Imported s_latest;

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
    return true;
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
        if (ReadPolyline (element, *path, imported.error))
            imported.path = std::move (path);
        break;
    }
    if (imported.path == nullptr && imported.error.empty ())
        imported.error = "select a Polyline in Archicad first";
    std::lock_guard<std::mutex> lock (s_mutex);
    imported.serial = s_latest.serial + 1;
    s_latest = std::move (imported);
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

} // namespace geomsrv::archviz::analysispathsource
