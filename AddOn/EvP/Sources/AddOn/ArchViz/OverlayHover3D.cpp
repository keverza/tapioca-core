// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings and each cost at
// least one. The camera is the census's decode, read; nothing here runs on the render thread.
// ArchViz/OverlayHover3D -- see the header.

#include "ArchViz/OverlayHover3D.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/Dxgi/CameraFreshness.hpp"
#include "ArchViz/Dxgi/CameraLayout.hpp"
#include "ArchViz/OverlayController.hpp"
#include "ArchViz/OverlayHover.hpp"
#include "ArchViz/OverlayVisibility.hpp"

#include <string>

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

void Fill (overlayhud::Input& input)
{
    if (!overlayvisibility::Hovering ())
        return;
    input.hover.picks = true; // the 3D view reads under the pointer: its readout may say "nothing"
    if (!input.pointer || !overlayvisibility::ContentShown ())
        return;

    freshness::CameraCopy camera;
    if (!freshness::LatestCamera (camera)) {
        Decline ("no camera decoded yet -- orbit the view once");
        return;
    }
    // ⚠️ ONLY A CAMERA: what the census admitted for the selected group can be the camera
    // draw holding something else (`camerachoice::CountsForGroup`); projected, it would
    // read nonsense.
    float decoded[16];
    if (cameralayout::Decode (camera.view, camera.projection, decoded) == cameralayout::Layout::Neither) {
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
    const overlayhover::Hit hit = overlayhover::PickView (layers, project, input.x, input.y);
    if (!hit.found)
        return;
    const overlaylayers::Mesh& mesh = layers[hit.layer]->meshes[hit.mesh];
    overlayhud::Hover hover = overlayhover::Readout (*layers[hit.layer], hit);
    hover.picks = true;
    const bool whole = mesh.values.empty () && mesh.indices.size () / 3 <= kTintTriangles;
    overlayhover::TintView (mesh, project, hover.tint, whole ? -1 : int64_t (hit.triangle));
    input.hover = std::move (hover);
}

} // namespace overlayhover3d
} // namespace archviz
} // namespace geomsrv
