#ifndef EVP_ARCHVIZ_DXGI_HOSTCOMPOSITE_HPP
#define EVP_ARCHVIZ_DXGI_HOSTCOMPOSITE_HPP

// Drawing the overlay INTO ARCHICAD'S OWN FRAME (PLAT-RE79, phase 4). Consumer
// half; SharedOverlaySurface is the producer.
//
// WHY, IN ONE PARAGRAPH. Every other rung has been measured and none of them
// moved the floor: DXGI frame latency 1 versus 3 changed nothing across five
// cells; the poll runs at 52-66 Hz with the ACAPI read costing 0.1-1.1 ms;
// prediction at scale 3 buys 4-6 ms and pays for it in overshoot. What is left
// is two swap chains composited by DWM on its own schedule, and the only
// construction that removes that is putting our pixels in the frame Archicad is
// already presenting. Phase 3 proved the injection point exists and is stable:
// 3166 rectangles into Archicad's back buffer, zero failures, no flicker,
// surviving resize and minimise/restore.
//
// ⚠️ THIS IS A DRAW, NOT A ClearView, AND THAT IS THE WHOLE RISK. Phase 3 got
// away with binding nothing. A textured quad binds a vertex shader, a pixel
// shader, an input layout, a blend state, a rasteriser state, a depth-stencil
// state, a viewport, a render target and a shader resource -- on ARCHICAD'S
// immediate context, between two of ITS draws. Every one of those must be read
// back before and restored after, or Archicad's next frame is corrupted in a way
// that looks like a driver fault. `imgui_impl_dx11.cpp` is the reference for
// exactly which fields; the RAII backup here is that list.
//
// ⚠️ NO VERTEX BUFFER AND NO INPUT LAYOUT, deliberately. The quad is a single
// oversized triangle generated from SV_VertexID, so there is no vertex buffer to
// bind, no input layout to create and two fewer pieces of Archicad's state to
// save. It also cannot be wrong about a stride.
//
// ⚠️ THE BLEND IS PREMULTIPLIED (ONE / INV_SRC_ALPHA). The shared texture is a
// copy of a composition back buffer, which is premultiplied by construction.
// Straight-alpha blending of premultiplied pixels darkens every edge and reads
// as a shader bug rather than a blend-state one.
//
// ⚠️ ARCHICAD'S RENDER THREAD, INSIDE THE PRESENT DETOUR. It allocates on the
// first frame only, takes no lock, logs nothing on the hot path and never calls
// ACAPI. Failures are counted with the step that refused; a compositor that
// cannot draw must never turn Archicad's Present into a crash.

#include <cstdint>
#include <string>

struct IDXGISwapChain;

namespace geomsrv {
namespace archviz {
namespace dxgi {

// RENDER THREAD (Archicad's). Called from the Present detour, before the
// original Present. No-op unless enabled, this is the nominated chain, and the
// producer has published a surface.
void CompositeOverlayIfTarget (IDXGISwapChain* swapChain);

// MAIN THREAD.
void SetHostCompositeEnabled (bool enabled);
bool HostCompositeEnabled ();

// Cheap enough for the detour: true once the pipeline is built and the shared
// surface is open. The marker squares draw only while this is false, so they
// double as "the hook is alive and the overlay is not arriving".
bool HostCompositeReady ();

// MAIN THREAD, from the camera-sync tick. The self-healing half of this mode.
//
// ⚠️ THE OVERLAY WINDOW IS HIDDEN WHILE hookdraw IS ARMED, so a compositor that
// stops producing leaves the user with NOTHING on screen and no way to get it
// back short of restarting Archicad. That is not hypothetical: a CANCELLED
// command cannot tear this mode down at all, because after a Stop the bus
// refuses every call the `finally` block makes. So the restore cannot live in
// Python. If the blit count stops advancing while the window is hidden, this
// puts the window back -- the overlay returns to the composition path on its
// own, and the worst case becomes "the overlay reappears" instead of "the
// screen is empty".
void WatchHostComposite ();

struct HostCompositeStats {
    bool        enabled = false;
    bool        ready = false;        // shaders built and the surface opened
    uint64_t    blits = 0;            // frames the overlay was drawn into
    uint64_t    framesConsumed = 0;   // new overlay frames actually picked up
    // Blits that WARPED the frame to the newest camera (PLAT-RE114). Far below
    // `blits` means the poses are not reaching the detour, and the composite is
    // merely late rather than corrected -- which looks identical on a still view.
    uint64_t    reprojections = 0;
    uint64_t    failures = 0;
    uint32_t    backBufferFormat = 0; // DXGI_FORMAT of Archicad's back buffer
    uint32_t    width = 0;            // the back buffer's, this frame
    uint32_t    height = 0;
    std::string lastError;
};
HostCompositeStats GetHostCompositeStats ();

}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv

#endif
