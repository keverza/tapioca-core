// See OverlayComposer.hpp.

#include "ArchViz/Dxgi/OverlayComposer.hpp"

#include "ArchViz/Dxgi/DepthCheckpoints.hpp"
#include "ArchViz/Dxgi/GhostMesh.hpp"
#include "ArchViz/Dxgi/HostOccluders.hpp"
#include "ArchViz/Dxgi/HostOverlay.hpp"
#include "ArchViz/Dxgi/OverlayStyle.hpp"

#include <d3d11_1.h>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace overlaycompose {

void Compose (ID3D11DeviceContext* context, ID3D11DeviceContext1* context1, uint32_t interpretation,
              ID3D11RenderTargetView* targetView, ID3D11DepthStencilView* depthView)
{
    // The moved block reads `wanted`, which is what the interpretation is called
    // on the other side of the seam.
    const uint32_t wanted = interpretation;

    // ---- the ghost mesh ----------------------------------------------------
    // ⚠️ AFTER THE PROOF PRIMITIVES AND WITH THE SAME CAMERA, THE SAME
    // DEPTH VIEW AND THE SAME DEPTH STATE. Everything that distinguishes this
    // draw from the one above is the geometry itself, so if the triangle
    // composites and the mesh does not, the fault is in the mesh and nowhere
    // else. The pipeline behind it belongs to `GhostMesh`, which is why this is
    // one line: a mesh bug cannot reach the instrument that would diagnose it.
    // ⚠️ THREE OVERLAY KINDS, ONE CAMERA, ONE FRAME, AND THE ORDER
    // IS THE POLICY. Solid first so its depth writes land; then the wireframe,
    // which tests against those writes and adds none of its own, so it is hidden
    // by a ghost cube but never by its own far edges; then the heatmap, biased
    // towards the camera because it is coplanar with the surface it describes.
    //
    // ⚠️ AND THE DEPTH VIEW THEY TEST AGAINST IS SEEDED FROM THE HOST
    // OCCLUDER, NOT BOUND AS A SECOND BUFFER. D3D11 binds one DSV; the inherited
    // values are what hides the overlay behind the building and our own writes
    // are what hides it behind itself.
    // ⚠️ THE HOST OCCLUDER IS RENDERED FIRST AND THE OVERLAY DRAWS
    // INTO ITS BUFFER. Opaque Archicad surfaces put their depth there; glass, the
    // build plane and every helper contribute nothing, so they cannot hide the
    // overlay -- which is the fault run forty-eight found and runs forty-nine to
    // fifty-one narrowed to a six-index quad that does not even write depth.
    //
    // Falling back to `depthView` when there is no host snapshot means the
    // overlay behaves exactly as it did before the extraction arrives, rather
    // than becoming un-occludable without saying so.
    ID3D11DepthStencilView* const hostView = hostocclusion::Prepare (context, context1, wanted);
    ID3D11DepthStencilView* const overlayView = hostView != nullptr ? hostView : depthView;
    if (targetView != nullptr)
        context->OMSetRenderTargets (1, &targetView, overlayView);

    // ⚠️ THE DRAW ORDER IS THE COMPOSITION; `HostOverlay.hpp` states
    // it once and this is it. Solid ghost first because it is the only overlay
    // that writes depth; host surfaces next; host edges last.
    if (ghost::Prepare (context) > 0) {
        ghost::DrawPart (context, wanted, ghost::Part::Solid, overlay::SolidGhostStyle (), overlayView);
        // The synthetic wireframe box and gradient grid: stand-ins from before
        // the host geometry existed, off by default. See `ghost::SetStandIns`.
        if (ghost::StandIns ()) {
            ghost::DrawPart (context, wanted, ghost::Part::Wireframe, overlay::WireframeStyle (), overlayView);
            ghost::DrawPart (context, wanted, ghost::Part::Heatmap, overlay::HeatmapStyle (), overlayView);
        }
    }

    if (hostView != nullptr) {
        if (hostoverlay::Enabled (hostoverlay::Kind::Heatmap)) {
            hostoverlay::Draw (context, context1, wanted, hostoverlay::Kind::Heatmap, overlay::HostHeatmapStyle (),
                               overlayView);
        }
        if (hostoverlay::Enabled (hostoverlay::Kind::Wireframe)) {
            hostoverlay::Draw (context, context1, wanted, hostoverlay::Kind::Wireframe, overlay::HostWireframeStyle (),
                               overlayView);
        }
    }

    // Collect any checkpoint occlusion results that became ready. The
    // diagnostic is disarmed by default and this costs one branch; see
    // DepthCheckpoints.hpp for what it answered and why it is off.
    injection::checkpoints::Resolve (context);
}

} // namespace overlaycompose
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
