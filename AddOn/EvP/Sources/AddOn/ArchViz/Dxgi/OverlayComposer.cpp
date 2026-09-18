// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.
// See OverlayComposer.hpp.

#include "ArchViz/Dxgi/OverlayComposer.hpp"

#include "ArchViz/Dxgi/DepthCheckpoints.hpp"
#include "ArchViz/Dxgi/GhostMesh.hpp"
#include "ArchViz/Dxgi/HostOccluders.hpp"
#include "ArchViz/Dxgi/HostOverlay.hpp"
#include "ArchViz/Dxgi/InjectedDiligentContext.hpp"
#include "ArchViz/Dxgi/OverlayStyle.hpp"

#include <d3d11_1.h>

#include <string>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace overlaycompose {
namespace {

Stats g_stats;

// The pixel extent behind a view, or 0x0 when it cannot be asked. Two COM calls,
// once per Present, to answer a question that decides whether anything is drawn.
void ViewExtent (ID3D11View* view, uint32_t& width, uint32_t& height)
{
    width = 0;
    height = 0;
    if (view == nullptr)
        return;
    ID3D11Resource* resource = nullptr;
    view->GetResource (&resource);
    if (resource == nullptr)
        return;
    ID3D11Texture2D* texture = nullptr;
    if (SUCCEEDED (resource->QueryInterface (__uuidof (ID3D11Texture2D), (void**) &texture)) && texture != nullptr) {
        D3D11_TEXTURE2D_DESC desc = {};
        texture->GetDesc (&desc);
        width = desc.Width;
        height = desc.Height;
        texture->Release ();
    }
    resource->Release ();
}

} // namespace

Stats GetStats ()
{
    return g_stats;
}

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
    // ⚠️ MEASURE THE SURFACE FIRST, THEN ASK FOR A DEPTH THAT
    // FITS IT. D3D11 refuses a binding whose render target and depth-stencil
    // differ in size by even one pixel, and Archicad's windowed swap chain
    // measured 2450 wide against a 2449-wide 3D view -- so the overlay drew
    // nothing in a window while every counter said it had, and worked in full
    // screen where the two happen to be equal. The host occluder's depth is ours
    // to size; see `hostocclusion::Prepare`.
    ++g_stats.passes;
    ViewExtent (targetView, g_stats.targetWidth, g_stats.targetHeight);

    // ---- the Diligent boundary, stage 2 of 5 -------------------------------
    // ⚠️ IT ATTACHES AND WRAPS AND DRAWS NOTHING, AND
    // THAT IS THE POINT OF THE STAGE. The two things that can break Archicad are
    // attaching to its device and holding a reference to its back buffer. Both
    // happen here; the draw does not, so a run that corrupts Archicad's frame
    // has exactly one suspect instead of two. Stage 3 adds one triangle.
    //
    // ⚠️ AND IT RUNS ONLY WHEN THE BACKEND IS SELECTED.
    // Default is `NativeD3D11`, so production composes exactly as it did before
    // this file was touched; the regression command is what turns it on. Section
    // 12b: composition stays at Present, and this changes WHAT draws, never
    // where or when -- not even that, yet.
    if (injecteddiligent::GetBackend () == injecteddiligent::Backend::Diligent) {
        if (!injecteddiligent::Attached ()) {
            ID3D11Device* device = nullptr;
            context->GetDevice (&device);
            std::string error;
            injecteddiligent::Attach (device, context1, error);
            if (device != nullptr)
                device->Release ();
        }
        // The wrapper is the measurement: how often Archicad's back-buffer
        // texture actually moves decides whether caching it is worth anything.
        injecteddiligent::WrapRenderTarget (targetView);
    }

    ID3D11DepthStencilView* const hostView =
        hostocclusion::Prepare (context, context1, wanted, g_stats.targetWidth, g_stats.targetHeight);
    ID3D11DepthStencilView* overlayView = hostView != nullptr ? hostView : depthView;

    // ⚠️ AND STILL FAIL CLOSED IF THEY DISAGREE. `depthView` is
    // the injection's own copy of Archicad's depth and is NOT resized here, so a
    // frame that falls back to it can still mismatch. Dropping the depth loses
    // occlusion and draws anyway: worse than the intent, better than an invisible
    // overlay, and the count says which frame it was.
    ViewExtent (overlayView, g_stats.depthWidth, g_stats.depthHeight);
    if (overlayView != nullptr && g_stats.targetWidth != 0 &&
        (g_stats.targetWidth != g_stats.depthWidth || g_stats.targetHeight != g_stats.depthHeight)) {
        ++g_stats.sizeMismatches;
        overlayView = nullptr;
    }

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
