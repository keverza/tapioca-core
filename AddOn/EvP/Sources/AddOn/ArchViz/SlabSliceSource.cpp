// ArchViz/SlabSliceSource -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/SlabSliceSource.hpp"

#include "Diagnostics/ApiError.hpp"        // DescribeErr - never print a bare GSErrCode
#include "NativeCommands/CommandUtils.hpp" // WalkPolygonRings: the one walk of a polygon memo

#include <cmath>

namespace geomsrv {
namespace archviz {
namespace slabsource {

namespace {

constexpr double kHalfPi = 1.57079632679489661923;

std::string Utf8 (const GS::UniString& text)
{
    return text.ToCStr (0, MaxUSize, CC_UTF8).Get ();
}

std::string Describe (GSErrCode err)
{
    return Utf8 (evp::DescribeErr (err));
}

// Edges trimmed off vertical. `edgeTrims` is indexed like `coords`: 1-based, each
// contour closed by a repeat of its first node, which is not an edge of its own.
uint32_t SlopedEdges (const API_ElementMemo& memo)
{
    if (memo.edgeTrims == nullptr || memo.pends == nullptr)
        return 0;
    const Int32 trims = Int32 (BMGetHandleSize ((GSHandle) memo.edgeTrims) / sizeof (API_EdgeTrim));
    const Int32 subs = Int32 (BMGetHandleSize ((GSHandle) memo.pends) / sizeof (Int32)) - 1;
    uint32_t sloped = 0;
    for (Int32 sub = 1; sub <= subs; ++sub) {
        const Int32 start = (*memo.pends)[sub - 1] + 1;
        const Int32 end = (*memo.pends)[sub];
        for (Int32 i = start; i < end && i < trims; ++i) {
            const API_EdgeTrim& trim = (*memo.edgeTrims)[i];
            if (trim.sideType == APIEdgeTrim_Horizontal ||
                (trim.sideType == APIEdgeTrim_CustomAngle && std::fabs (trim.sideAngle - kHalfPi) > 1e-3))
                ++sloped;
        }
    }
    return sloped;
}

// The polygon, its holes and its trims, or false with `reason`.
bool ReadPolygon (const API_Guid& guid, slabslices::Slab& slab, std::string& reason)
{
    API_ElementMemo memo = {};
    const GSErrCode err = ACAPI_Element_GetMemo (guid, &memo, APIMemoMask_Polygon | APIMemoMask_EdgeTrims);
    if (err != NoError) {
        ACAPI_DisposeElemMemoHdls (&memo);
        reason = "its polygon could not be read (ACAPI_Element_GetMemo: " + Describe (err) + ")";
        return false;
    }
    PolygonHandles polygon;
    polygon.coords = memo.coords;
    polygon.pends = memo.pends;
    polygon.parcs = memo.parcs;
    GS::Array<double> outer, outerArcs, holes, holeArcs;
    GS::Array<GS::Int32> holeCounts;
    GS::Int32 outerCount = 0, holeTotal = 0;
    const bool walked =
        WalkPolygonRings (polygon, nullptr, outer, outerArcs, outerCount, holes, holeArcs, holeCounts, holeTotal);
    slab.slopedEdges = SlopedEdges (memo);
    ACAPI_DisposeElemMemoHdls (&memo);
    if (!walked || outerCount < 2) {
        reason = "its polygon has no outline";
        return false;
    }
    for (GS::Int32 i = 0; i < outerCount; ++i) {
        slab.outer.xy.push_back (outer[UIndex (i * 2)]);
        slab.outer.xy.push_back (outer[UIndex (i * 2 + 1)]);
        slab.outer.arcs.push_back (outerArcs[UIndex (i)]);
    }
    GS::Int32 cursor = 0;
    for (GS::Int32 h = 0; h < holeTotal; ++h) {
        slabslices::Ring ring;
        for (GS::Int32 i = 0; i < holeCounts[UIndex (h)]; ++i) {
            const UIndex v = UIndex (cursor + i);
            ring.xy.push_back (holes[v * 2]);
            ring.xy.push_back (holes[v * 2 + 1]);
            ring.arcs.push_back (holeArcs[v]);
        }
        cursor += holeCounts[UIndex (h)];
        slab.holes.push_back (std::move (ring));
    }
    return true;
}

} // namespace

bool Selected (std::vector<std::string>& guids, std::string& error)
{
    guids.clear ();
    API_SelectionInfo info = {};
    GS::Array<API_Neig> neigs;
    const GSErrCode err = ACAPI_Selection_Get (&info, &neigs, false);
    if (info.marquee.coords != nullptr)
        BMKillHandle (reinterpret_cast<GSHandle*> (&info.marquee.coords));
    if (err == APIERR_NOSEL)
        return true;
    if (err != NoError) {
        error = "the selection could not be read (ACAPI_Selection_Get: " + Describe (err) + ")";
        return false;
    }
    for (const API_Neig& neig : neigs) {
        const std::string guid = Utf8 (APIGuidToString (neig.guid));
        bool seen = false;
        for (const std::string& known : guids)
            seen = seen || known == guid;
        if (!seen)
            guids.push_back (guid);
    }
    return true;
}

Reading Read (const std::vector<std::string>& guids, const ProjectStoreys& storeys)
{
    Reading reading;
    for (const std::string& text : guids) {
        API_Element element = {};
        element.header.guid = APIGuidFromString (text.c_str ());
        const GSErrCode err = ACAPI_Element_Get (&element);
        if (err != NoError) {
            reading.skipped.push_back ({ text, "not found (ACAPI_Element_Get: " + Describe (err) + ")" });
            continue;
        }
        if (element.header.type.typeID != API_SlabID) {
            GS::UniString typeName;
            ACAPI_Element_GetElemTypeName (element.header.type, typeName);
            reading.skipped.push_back ({ text, Utf8 (typeName) + " - not a slab" });
            continue;
        }
        size_t home = storeys.indices.size ();
        for (size_t i = 0; i < storeys.indices.size (); ++i)
            if (storeys.indices[i] == int (element.header.floorInd))
                home = i;
        if (home == storeys.indices.size ()) {
            reading.skipped.push_back (
                { text, "its home storey " + std::to_string (element.header.floorInd) + " is not in the project" });
            continue;
        }
        slabslices::Slab slab;
        slab.guid = text;
        slab.top = storeys.levels[home] + element.slab.level + element.slab.offsetFromTop;
        slab.bottom = slab.top - element.slab.thickness;
        GS::UniString id;
        if (ACAPI_Element_GetElementInfoString (&element.header.guid, &id) == NoError)
            slab.id = Utf8 (id);
        std::string reason;
        if (!ReadPolygon (element.header.guid, slab, reason)) {
            reading.skipped.push_back ({ text, reason });
            continue;
        }
        reading.slabs.push_back (std::move (slab));
    }
    return reading;
}

std::vector<uint64_t> Stamps (const std::vector<std::string>& guids)
{
    std::vector<uint64_t> stamps;
    stamps.reserve (guids.size ());
    for (const std::string& text : guids) {
        API_Elem_Head head = {};
        head.guid = APIGuidFromString (text.c_str ());
        stamps.push_back (ACAPI_Element_GetHeader (&head) == NoError ? uint64_t (head.modiStamp) : 0);
    }
    return stamps;
}

} // namespace slabsource
} // namespace archviz
} // namespace geomsrv
