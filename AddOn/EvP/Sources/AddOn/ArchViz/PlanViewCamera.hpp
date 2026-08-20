#ifndef EVP_ARCHVIZ_PLANVIEWCAMERA_HPP
#define EVP_ARCHVIZ_PLANVIEWCAMERA_HPP

// ArchViz/PlanViewCamera — the ACAPI half of the plan overlay's camera.
//
// It asks Archicad where three of the overlay's own pixels land in model space
// and hands the answer to PlanCameraMath, which turns it into a top-down
// orthographic pose. Read that header first: it says why the mapping is
// MEASURED rather than derived from `ACAPI_View_GetZoom`'s box.
//
// ⚠️ MAIN THREAD ONLY. `ACAPI_Window_GetCurrentWindow`, `ACAPI_View_PointToCoord`
// and `ACAPI_View_GetZoom` are all ACAPI, and every one of them answers for THE
// CURRENT DATABASE -- which is a main-thread notion. The render thread must
// never call this; the palette's camera-sync timer is the one caller, and it is
// a Win32 timer dispatched on the main thread for exactly this reason
// (ArchVizPanel::StartCameraSync).

#include "ArchViz/DiligentViewport.hpp"   // CameraStart

#include <cstdint>

namespace geomsrv {
namespace archviz {

// Is the window Archicad currently has in front a 2D DRAWING window -- one whose
// contents are a projection of the model onto a plane that PointToCoord can
// speak about?
//
// ⚠️ IT IS THE FLOOR PLAN AND NOTHING ELSE, FOR NOW, AND THAT IS DELIBERATE. A
// section or elevation window also has a pixel-to-coordinate mapping, but its
// coordinates are in the SECTION's own 2D space, not the model's -- so the same
// three samples would produce a confident, completely wrong camera. Adding one
// means establishing what that window's coordinates mean first, empirically, the
// way the plan's were.
bool CurrentWindowIsFloorPlan ();

// A top-down orthographic camera matching the frontmost floor-plan window, or
// `valid == false` with the reason in `source`.
//
// `widthPx`/`heightPx` are the OVERLAY's surface size -- the same rectangle the
// picture is going to be drawn into. See PlanCameraMath.hpp for why it must be
// that one and not, say, the zoom box's aspect.
CameraStart ReadPlanViewCamera (uint32_t widthPx, uint32_t heightPx);

}   // namespace archviz
}   // namespace geomsrv

#endif
