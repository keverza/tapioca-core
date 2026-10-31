#include "APIEnvir.h"
#include "ACAPinc.h"

// Bound by OVERLAY-INVARIANTS.md (section 5) with ArchViz/ModelWatch: see the header.
#include "ArchViz/ModelStampWalk.hpp"

#include "Diagnostics/ApiError.hpp" // EVP_ACAPI_FAIL -- never print a bare GSErrCode

#include <chrono>

namespace geomsrv {
namespace archviz {
namespace stampwalk {

namespace {

// Element types that never draw in the 3D window (DevKit enum, so not in the
// header the tests include). Their edits must not
// start an extraction and a sun-study refresh for nothing.
inline bool DrawsNoGeometry (API_ElemTypeID type)
{
    switch (type) {
        case API_DimensionID:
        case API_RadialDimensionID:
        case API_LevelDimensionID:
        case API_AngleDimensionID:
        case API_TextID:
        case API_LabelID:
        case API_HatchID:
        case API_LineID:
        case API_PolyLineID:
        case API_ArcID:
        case API_CircleID:
        case API_SplineID:
        case API_HotspotID:
        case API_CutPlaneID:
        case API_CameraID:
        case API_CamSetID:
        case API_GroupID:
        case API_SectElemID:
        case API_DrawingID:
        case API_PictureID:
        case API_DetailID:
        case API_ElevationID:
        case API_InteriorElevationID:
        case API_WorksheetID:
        case API_ChangeMarkerID:
            return true;
        default:
            return false;
    }
}

} // namespace

modeldiff::Result Walker::Poll (bool reset)
{
    modeldiff::Result result;
    const auto started = std::chrono::steady_clock::now ();
    GS::Array<API_Guid> guids;
    const GSErrCode listErr = ACAPI_Element_GetElemList (API_ZombieElemID, &guids, APIFilt_None);
    const auto listed = std::chrono::steady_clock::now ();
    listMs_ = std::chrono::duration_cast<std::chrono::milliseconds> (listed - started).count ();
    if (listErr != NoError) {
        result.error =
            EVP_ACAPI_FAIL ("ACAPI_Element_GetElemList", listErr, "listing the elements the 3D shows").ToCStr ().Get ();
        return result;
    }
    Stamps now;
    for (const API_Guid& guid : guids) {
        API_Elem_Head head = {};
        head.guid = guid;
        // An element gone between the list and its header is simply not there.
        if (ACAPI_Element_GetHeader (&head) == NoError && !DrawsNoGeometry (head.type.typeID))
            now.emplace (APIGuidToString (guid).ToCStr ().Get (), uint64_t (head.modiStamp));
    }
    result.firstCall = !have_ || reset;
    if (!result.firstCall)
        Compare (stamps_, now, result);
    stamps_ = std::move (now);
    have_ = true;
    result.ok = true;
    const auto finished = std::chrono::steady_clock::now ();
    headerMs_ = std::chrono::duration_cast<std::chrono::milliseconds> (finished - listed).count ();
    result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds> (finished - started).count ();
    return result;
}

bool ElementBox (const std::string& guid, Box& box)
{
    API_Elem_Head head = {};
    head.guid = APIGuidFromString (guid.c_str ());
    if (ACAPI_Element_GetHeader (&head) != NoError)
        return false;
    API_Box3D extent = {};
    if (ACAPI_Element_CalcBounds (&head, &extent) != NoError)
        return false;
    box.lo[0] = extent.xMin;
    box.lo[1] = extent.yMin;
    box.lo[2] = extent.zMin;
    box.hi[0] = extent.xMax;
    box.hi[1] = extent.yMax;
    box.hi[2] = extent.zMax;
    return true;
}

} // namespace stampwalk
} // namespace archviz
} // namespace geomsrv
