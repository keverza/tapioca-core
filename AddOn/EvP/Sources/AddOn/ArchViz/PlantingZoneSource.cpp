// ArchViz/PlantingZoneSource -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/AnalysisPath.hpp" // Build: the one arc tessellation of a plan outline
#include "ArchViz/PlantingZoneSource.hpp"
#include "ArchViz/SelectionMetadata.hpp"
#include "ArchViz/SlabSliceSource.hpp"

#include "Diagnostics/ApiError.hpp"        // DescribeErr - never print a bare GSErrCode
#include "NativeCommands/CommandUtils.hpp" // WalkPolygonRings: the one walk of a polygon memo

#include <mutex>
#include <vector>

namespace geomsrv::archviz::plantingzonesource {

namespace {

std::mutex s_mutex;
Imported s_latest;

std::string Describe (GSErrCode err)
{
    return evp::DescribeErr (err).ToCStr (0, MaxUSize, CC_UTF8).Get ();
}

// One ring as x, y pairs, arcs tessellated, the closing point dropped.
std::vector<double> Ring (const std::string& guid, const GS::Array<double>& coords, const GS::Array<double>& arcs,
                          GS::Int32 first, GS::Int32 count)
{
    std::vector<double> xy, bulge;
    for (GS::Int32 i = 0; i < count; ++i) {
        xy.push_back (coords[UIndex ((first + i) * 2)]);
        xy.push_back (coords[UIndex ((first + i) * 2 + 1)]);
        bulge.push_back (arcs[UIndex (first + i)]);
    }
    const analysispath::Path path = analysispath::Build (guid, xy, bulge, true, 0.0);
    std::vector<double> ring;
    for (size_t i = 0; i + 1 < path.points.size (); ++i) { // a closed path ends on its first point
        ring.push_back (path.points[i][0]);
        ring.push_back (path.points[i][1]);
    }
    return ring;
}

// MAIN THREAD.
bool ReadZone (const API_Element& element, planting::PlantingZone& zone, std::string& reason)
{
    const bool polyline = element.header.type.typeID == API_PolyLineID;
    API_ElementMemo memo {};
    const GSErrCode err = ACAPI_Element_GetMemo (element.header.guid, &memo, APIMemoMask_Polygon);
    if (err != NoError) {
        ACAPI_DisposeElemMemoHdls (&memo);
        reason = "not read (ACAPI_Element_GetMemo: " + Describe (err) + ")";
        return false;
    }
    PolygonHandles polygon;
    polygon.coords = memo.coords;
    polygon.pends = memo.pends;
    polygon.parcs = memo.parcs;
    GS::Array<double> outer, outerArcs, holes, holeArcs;
    GS::Array<GS::Int32> holeCounts;
    GS::Int32 outerCount = 0, holeTotal = 0;
    bool closed = !polyline;
    const bool walked = WalkPolygonRings (polygon, nullptr, outer, outerArcs, outerCount, holes, holeArcs, holeCounts,
                                          holeTotal, polyline, polyline ? &closed : nullptr);
    ACAPI_DisposeElemMemoHdls (&memo);
    // Two nodes are enough: a circle is two nodes and two 180-degree arcs.
    if (!walked || outerCount < 2) {
        reason = "fewer than two points";
        return false;
    }
    if (!closed) {
        reason = "not closed";
        return false;
    }
    zone.guid = APIGuidToString (element.header.guid).ToCStr ().Get ();
    zone.fill = !polyline;
    zone.stamp = uint64_t (element.header.modiStamp);
    zone.rings.push_back (Ring (zone.guid, outer, outerArcs, 0, outerCount));
    if (zone.rings[0].size () < 6) {
        reason = "no area";
        return false;
    }
    GS::Int32 at = 0;
    for (GS::Int32 h = 0; h < holeTotal && UIndex (h) < holeCounts.GetSize (); ++h) {
        const GS::Int32 count = holeCounts[UIndex (h)];
        zone.rings.push_back (Ring (zone.guid, holes, holeArcs, at, count));
        at += count;
    }
    GS::UniString id;
    if (ACAPI_Element_GetElementInfoString (&element.header.guid, &id) == NoError && !id.IsEmpty ())
        zone.name = id.ToCStr (0, MaxUSize, CC_UTF8).Get ();
    else
        zone.name = std::string (polyline ? "Polyline .." : "Fill ..") +
                    zone.guid.substr (zone.guid.size () >= 4 ? zone.guid.size () - 4 : 0);
    return true;
}

// MAIN THREAD.
void Import ()
{
    Imported imported;
    std::vector<std::string> guids;
    if (!slabsource::Selected (guids, imported.error)) {
        std::lock_guard<std::mutex> lock (s_mutex);
        imported.serial = s_latest.serial + 1;
        s_latest = std::move (imported);
        return;
    }
    for (const auto& guid : guids) {
        API_Element element {};
        element.header.guid = APIGuidFromString (guid.c_str ());
        if (ACAPI_Element_Get (&element) != NoError)
            continue;
        const API_ElemTypeID type = element.header.type.typeID;
        if (type != API_PolyLineID && type != API_HatchID)
            continue;
        planting::PlantingZone zone;
        std::string reason;
        if (imported.zones.size () >= planting::kMaxPlantingZones)
            imported.skipped.push_back (guid + ": more than 32 zones");
        else if (ReadZone (element, zone, reason))
            imported.zones.push_back (std::move (zone));
        else
            imported.skipped.push_back (std::string (type == API_PolyLineID ? "Polyline .." : "Fill ..") +
                                        guid.substr (guid.size () >= 4 ? guid.size () - 4 : 0) + ": " + reason);
    }
    if (imported.zones.empty () && imported.error.empty ())
        imported.error = "select closed Polylines or Fills in Archicad first";
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
    s_latest.zones.clear ();
    s_latest.error = "the import was not queued: the project message loop is unavailable";
}

Imported Latest ()
{
    std::lock_guard<std::mutex> lock (s_mutex);
    return s_latest;
}

} // namespace geomsrv::archviz::plantingzonesource
