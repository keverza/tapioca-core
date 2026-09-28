// ⚠️ BOUND BY OVERLAY-INVARIANTS.md, finding 14 and §11's one exception: this read is
// taken inside the floor plan's Present, and only there is it the frame's transform.
// ArchViz/PlanViewTransform -- see the header. MAIN THREAD ONLY.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/PlanViewTransform.hpp"

#include "PlanOverlay/PlanTransformMath.hpp"

namespace geomsrv {
namespace archviz {

namespace {

bool Sample (uint32_t x, uint32_t y, planoverlay::PlanPoint& out, int32_t& error)
{
    API_Point point = {};
    point.h = short (x);
    point.v = short (y);
    API_Coord coord = {};
    const GSErrCode result = ACAPI_View_PointToCoord (&point, &coord);
    if (result != NoError) {
        error = int32_t (result);
        return false;
    }
    out.x = coord.x;
    out.y = coord.y;
    return true;
}

} // namespace

PlanViewTransform ReadPlanViewTransform (uint32_t logicalWidth, uint32_t logicalHeight)
{
    PlanViewTransform result;
    // `API_Point` is a pair of shorts: a wider canvas would wrap silently.
    if (logicalWidth < 2 || logicalHeight < 2 || logicalWidth > 32000 || logicalHeight > 32000)
        return result;
    planoverlay::PlanPoint topLeft, topRight, bottomLeft, topLeftAgain;
    if (!Sample (0, 0, topLeft, result.error) || !Sample (logicalWidth, 0, topRight, result.error) ||
        !Sample (0, logicalHeight, bottomLeft, result.error) || !Sample (0, 0, topLeftAgain, result.error))
        return result;
    const planoverlay::LogicalSampleRect rect =
        planoverlay::MakeLogicalSampleRect (double (logicalWidth), double (logicalHeight), 1.0);
    const planoverlay::PlanTransform transform =
        planoverlay::ObservePlanTransform (rect, topLeft, topRight, bottomLeft, topLeftAgain);
    result.valid = transform.valid;
    result.torn = !transform.valid && transform.tearPixels > 0.25;
    result.xx = transform.xx;
    result.xy = transform.xy;
    result.yx = transform.yx;
    result.yy = transform.yy;
    result.ox = transform.offsetX;
    result.oy = transform.offsetY;
    return result;
}

} // namespace archviz
} // namespace geomsrv
