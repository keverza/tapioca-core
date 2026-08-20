#ifndef EVP_ARCHVIZ_DXGI_HOOKMARKER_HPP
#define EVP_ARCHVIZ_DXGI_HOOKMARKER_HPP

// Drawing ONE FIXED SQUARE into Archicad's own back buffer (PLAT-RE79, phase 3).
//
// WHY THIS EXISTS AT ALL, AND WHY IT DRAWS SOMETHING THIS USELESS. Every rung of
// the camera-sync ladder has now been measured, and the measurements agree: the
// overlay's camera desync sits at 10-18 ms and will not go lower. DXGI frame
// latency 1 versus 3 changed nothing (2026-08-14, both arms, five cells); the
// poll runs at 52-66 Hz with the ACAPI read costing 0.1-1.1 ms, so the samples
// are not starved; prediction at scale 3 buys 4-6 ms of lag back and pays for it
// in residual error. What is left is structural: two swap chains, composited by
// DWM on its own schedule, cannot be made to land in the same frame. The only
// construction that removes it is drawing into the frame Archicad is already
// presenting -- which is this file's whole subject.
//
// ⚠️ BEFORE ANY OF THAT IS WORTH BUILDING, ONE QUESTION HAS TO BE ANSWERED ON
// ITS OWN: can we put ANY pixel into Archicad's back buffer, reliably, without
// destabilising Archicad? A fixed square answers exactly that and nothing else.
// It needs no camera, no model, no projection and no Diligent device, so when it
// misbehaves there is only one candidate cause. If a square that never moves
// cannot be made to sit still, nothing camera-driven ever will, and the host-hook
// path stops here rather than after weeks of renderer work.
//
// ⚠️ IT IS A `ClearView` CALL, NOT A DRAW, and that is the entire risk argument.
// A real draw would bind a shader, an input layout, a vertex buffer, a viewport,
// a blend state and a depth state -- six pieces of Archicad's pipeline state to
// save and restore exactly, on a thread we do not own, where getting one wrong
// corrupts Archicad's next frame in a way that looks like a driver bug.
// ID3D11DeviceContext1::ClearView fills a rectangle of a view with a colour and
// binds nothing, so there is no state to back up and none to restore. Phase 3
// asks whether the injection point works; it does not need a pipeline to ask.
//
// ⚠️ THE TARGET SWAP CHAIN IS CHOSEN, NEVER ASSUMED. The vtable is shared by
// every swap chain in the process, so the detour sees ours too, and a process
// this size may have others (a preview pane, a video control). Drawing into the
// wrong one puts a red square somewhere unrelated -- or into our own overlay,
// which would look exactly like success. The marker draws only into a chain that
// has been explicitly nominated by the main thread, and 0 means "nobody yet, do
// nothing".
//
// ⚠️ THE RENDER-TARGET VIEW IS CREATED PER FRAME AND NOT CACHED. With a flip-
// model swap chain, buffer 0 is a different surface after every Present, so a
// cached view would address a buffer that is no longer the back buffer -- the
// square would land on an old frame or nowhere, intermittently, which is the
// worst possible failure for a test whose verdict is "does it sit still".
//
// ⚠️ RENDER THREAD, INSIDE THE DETOUR. It calls into D3D on the thread that was
// already about to present, which is the only thread allowed to touch that
// immediate context. It allocates nothing, takes no lock, logs nothing and never
// calls ACAPI. Failures are counted and the reason is kept for the main thread
// to report; a marker that cannot draw must never turn a present into a crash.

#include <cstdint>
#include <string>

struct IDXGISwapChain;

namespace geomsrv {
namespace archviz {
namespace dxgi {

// RENDER THREAD. Called from the Present detour before the original Present.
// Does nothing unless the marker is enabled AND this is the nominated chain.
void DrawMarkerIfTarget (IDXGISwapChain* swapChain);

// MAIN THREAD. Enabling with no target nominated draws nothing until one is.
void SetMarkerEnabled (bool enabled);
bool MarkerEnabled ();

// MAIN THREAD. Nominate the chain to draw into; 0 disables drawing. See the
// header comment on why this is nominated rather than inferred.
void SetMarkerTarget (uint64_t swapChain);
uint64_t MarkerTarget ();

// MAIN THREAD. Nominate the busiest chain that is not ours, if one is known and
// none has been nominated yet. Called from the camera-sync tick because the
// answer needs a second of frames to exist, and blocking the main thread that
// long inside the arm call would stall Archicad's UI.
void ChooseMarkerTargetIfUnset ();

struct MarkerStats {
    bool        enabled = false;
    uint64_t    target = 0;
    uint64_t    draws = 0;      // rectangles actually filled
    uint64_t    failures = 0;   // frames where a D3D step refused
    std::string lastError;      // the first step that refused, most recently
};
MarkerStats GetMarkerStats ();

}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv

#endif
