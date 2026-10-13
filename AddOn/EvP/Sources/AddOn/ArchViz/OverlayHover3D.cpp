// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings and each cost at
// least one. The camera is the census's decode, read; nothing here runs on the render thread.
// ArchViz/OverlayHover3D -- see the header.

#include "ArchViz/OverlayHover3D.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/Dxgi/CameraFreshness.hpp"
#include "ArchViz/Dxgi/CameraLayout.hpp"
#include "ArchViz/OverlayController.hpp"
#include "ArchViz/OverlayHover.hpp"
#include "ArchViz/OverlayHoverDwell.hpp"
#include "ArchViz/OverlayInput.hpp"
#include "ArchViz/OverlayVisibility.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <windows.h>

namespace geomsrv {
namespace archviz {
namespace overlayhover3d {

namespace {

namespace freshness = dxgi::injection::freshness;
namespace cameralayout = dxgi::cameralayout;

// A mesh larger than this tints only the triangle hit, as in the plan.
constexpr size_t kTintTriangles = 4096;

// ⚠️ A POINTER NOT READ SAYS WHY, ONCE (§7): the reason it last could not, cleared when it can.
std::string g_declined;
CellDwell s_dwell;
std::shared_ptr<const overlaylayers::Layer> s_dwellLayer;
UINT_PTR s_dwellTimer = 0;

void CALLBACK DwellReady (HWND, UINT, UINT_PTR timer, DWORD)
{
    if (timer != s_dwellTimer)
        return;
    ::KillTimer (nullptr, timer);
    s_dwellTimer = 0;
    // Refresh through the existing main-thread input owner; a stationary pointer
    // must not depend on a second mouse move or the coarse controller heartbeat.
    overlayinput::RequestLayout (overlayinput::View::ThreeD);
}

void Decline (const char* why)
{
    if (g_declined == why)
        return;
    g_declined = why;
    ArchVizLog (std::string ("HOVER 3D     the pointer is not read: ") + why);
}

} // namespace

bool Hovering ()
{
    return overlayvisibility::Hovering ();
}

void ResetDwell ()
{
    if (s_dwellTimer != 0)
        ::KillTimer (nullptr, s_dwellTimer);
    s_dwellTimer = 0;
    s_dwell.Reset ();
    s_dwellLayer.reset ();
}

void Fill (overlayhud::Input& input)
{
    if (!overlayvisibility::Hovering ()) {
        ResetDwell ();
        return;
    }
    input.hover.picks = true; // the 3D view reads under the pointer: its readout may say "nothing"
    if (!input.pointer || input.overHud || !overlayvisibility::ContentShown ()) {
        ResetDwell ();
        return;
    }

    freshness::CameraCopy camera;
    if (!freshness::LatestCamera (camera)) {
        ResetDwell ();
        Decline ("no camera decoded yet -- orbit the view once");
        return;
    }
    // ⚠️ ONLY A CAMERA: what the census admitted for the selected group can be the camera
    // draw holding something else (`camerachoice::CountsForGroup`); projected, it would
    // read nonsense.
    float decoded[16];
    if (cameralayout::Decode (camera.view, camera.projection, decoded) == cameralayout::Layout::Neither) {
        ResetDwell ();
        Decline ("the last capture does not decode as a camera");
        return;
    }
    if (!g_declined.empty ()) {
        ArchVizLog ("HOVER 3D     the pointer is read again");
        g_declined.clear ();
    }

    // x, y and w as the shaders draw them: T(-eye) * b0 (cameralayout, finding 2).
    double viewProjection[16];
    cameralayout::ViewProjection (camera.projection, camera.view, viewProjection);
    const overlayhover::ProjectView project = [&] (double x, double y, double z, float& px, float& py, float& invW) {
        return overlayhover::ProjectThrough (viewProjection, camera.viewport, x, y, z, px, py, invW);
    };
    const std::vector<std::shared_ptr<const overlaylayers::Layer>> layers = overlaycontrol::ShownLayers ();
    const overlayhover::Hit hit = overlayhover::PickView (
        layers, project, input.x, input.y, overlayhover::ProjectionKey (viewProjection, camera.viewport));
    if (!hit.found) {
        ResetDwell ();
        return;
    }
    const overlaylayers::Mesh& mesh = layers[hit.layer]->meshes[hit.mesh];
    overlayhud::Hover hover = overlayhover::Readout (*layers[hit.layer], hit);
    if (hover.hasMeasuredCell && hit.hasCell) {
        const uint64_t now = ::GetTickCount64 ();
        hover.measurementReady = s_dwell.Observe (reinterpret_cast<uintptr_t> (layers[hit.layer].get ()),
                                                 hit.mesh, hit.cell, now);
        s_dwellLayer = layers[hit.layer];
        if (!hover.measurementReady)
            s_dwellTimer = ::SetTimer (nullptr, s_dwellTimer, UINT ((std::max) (uint64_t (10), s_dwell.Remaining (now))),
                                      &DwellReady);
        else if (s_dwellTimer != 0) {
            ::KillTimer (nullptr, s_dwellTimer);
            s_dwellTimer = 0;
        }
    }
    else
        ResetDwell ();
    hover.picks = true;
    const bool whole = mesh.values.empty () && mesh.indices.size () / 3 <= kTintTriangles;
    // In model metres: the guest draws it with the camera of every Present, through an orbit.
    if (hit.hasCell && hit.cell < mesh.hoverCells.size ())
        overlayhover::TintModel (mesh, hover.tintModel, mesh.hoverCells[hit.cell].triangles);
    else
        overlayhover::TintModel (mesh, hover.tintModel, whole ? -1 : int64_t (hit.triangle));
    input.hover = std::move (hover);
}

void Projection (overlayhud::Input& input)
{
    freshness::CameraCopy camera;
    if (!freshness::LatestCamera (camera))
        return;
    float decoded[16];
    if (cameralayout::Decode (camera.view, camera.projection, decoded) == cameralayout::Layout::Neither)
        return;
    std::array<double, 16> viewProjection {};
    cameralayout::ViewProjection (camera.projection, camera.view, viewProjection.data ());
    std::array<float, 4> viewport {};
    std::copy (std::begin (camera.viewport), std::end (camera.viewport), viewport.begin ());
    // This captured projection's homogeneous-w column points into the visible half-space
    // (ProjectThrough accepts w > 0), so it is the camera-to-scene direction in model space.
    input.hasCameraDirection = true;
    input.cameraDirection[0] = camera.projection[3];
    input.cameraDirection[1] = camera.projection[7];
    input.cameraDirection[2] = camera.projection[11];
    input.project = [viewProjection, viewport] (double x, double y, double z, float& px, float& py) {
        float invW = 0.0f;
        return overlayhover::ProjectThrough (viewProjection.data (), viewport.data (), x, y, z, px, py, invW);
    };
}

} // namespace overlayhover3d
} // namespace archviz
} // namespace geomsrv
