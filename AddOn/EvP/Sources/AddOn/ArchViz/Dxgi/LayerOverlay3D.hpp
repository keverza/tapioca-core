#ifndef EVP_ARCHVIZ_DXGI_LAYEROVERLAY3D_HPP
#define EVP_ARCHVIZ_DXGI_LAYEROVERLAY3D_HPP

// ArchViz/Dxgi/LayerOverlay3D -- the caller's layers (ArchViz/OverlayLayers.hpp) in the
// 3D overlay: lines and filled triangles in world metres, drawn at Archicad's Present
// with the camera copied at the model's own draw -- exactly as the reference
// wireframe is, so they move with the model in the same frame.
// Bound by private/docs/architecture/diligent/OVERLAY-INVARIANTS.md (§2, §11).
//
// ⚠️ THE CAMERA IS THE WIREFRAME'S, NOT A COPY OF IT. The shader is composed through
// `camerashader::Compose` for the interpretation the census selected, binds the same
// two snapshot windows at `b1` and `b2`, and calls `ArchicadClip`; nothing here knows
// a matrix layout. A layer occludes against the host occluder's depth like the
// wireframe, or draws over everything when the caller asked for that.
//
// ⚠️ THE HAND-OVER IS ONE ATOMIC EXCHANGE EACH WAY, NEVER A LOCK (HostOccluders.hpp).
// The main thread publishes a complete prepared snapshot; the render thread takes
// ownership of it at its next composition and uploads it there, as the host
// occluder's own geometry is uploaded.
//
// THREADS. `Publish` is MAIN THREAD. `Draw` is Archicad's render thread inside
// `overlaycompose::Compose`, which has already saved the pipeline state and bound
// the target and depth. `ReleaseDeviceObjects` runs from `injection::Shutdown`, once
// no Present can reach this file.

#include "ArchViz/OverlayLayers.hpp"

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11DeviceContext1;
struct ID3D11DepthStencilView;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace layers3d {

// MAIN THREAD. What to draw from the next composition on.
void Publish (overlaylayers::Prepared3D prepared);

// RENDER THREAD, under the composer's guards, after the host edges.
void Draw (ID3D11DeviceContext* context, ID3D11DeviceContext1* context1, uint32_t interpretation,
           ID3D11DepthStencilView* depthView);

// Every device object. What was published is kept, and uploaded again on the next
// session's first composition.
void ReleaseDeviceObjects ();

struct Stats {
    uint64_t generation = 0; // of the snapshot being drawn
    uint32_t lineVertices = 0;
    uint32_t fillVertices = 0;
    uint64_t draws = 0;
    uint64_t skippedNoCamera = 0;
    uint64_t uploads = 0;
    char lastError[160] = {};
};
Stats GetStats ();

} // namespace layers3d
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
