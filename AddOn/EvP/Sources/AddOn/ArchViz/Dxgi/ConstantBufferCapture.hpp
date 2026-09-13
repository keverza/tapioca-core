#ifndef EVP_ARCHVIZ_DXGI_CONSTANTBUFFERCAPTURE_HPP
#define EVP_ARCHVIZ_DXGI_CONSTANTBUFFERCAPTURE_HPP

// INTERNAL to the view-matrix search: getting the BYTES out of Archicad's
// constant buffers (PLAT-RE155, docs/architecture/api/HANDOFF-OverlayPatch.md
// stage 3). What those bytes MEAN is `ViewMatrixCandidates`.
//
// WHY IT IS ITS OWN FILE. The two halves answer different questions and grow at
// different rates. This one is about D3D11: which resources are constant
// buffers, when they are mapped, where in a ring a bind points, and how to copy
// a kilobyte off a write-combined pointer before `Unmap` takes it away. The
// other is about linear algebra and scoring, and it doubled in a day when the
// classifier learned to multiply pairs. Together they went over the size cap;
// the seam between them was already the function call they share.
//
// ⚠️ EVERYTHING HERE RUNS ON ARCHICAD'S RENDER THREAD, inside a detour, and
// obeys that thread's rules: atomics only, no lock, no allocation, no ACAPI.
// `ContextHook.hpp` has the reasoning.

#include <cstdint>

struct ID3D11Buffer;
struct ID3D11Resource;

namespace geomsrv {
namespace archviz {
namespace dxgi {

struct CandidateStats;

namespace viewmatrix {

// The resource's constant-buffer byte width, or 0 for anything that is not one.
//
// ⚠️ ONE COM CALL PER RESOURCE POINTER, EVER, AND THE ANSWER IS CACHED. Asking
// `GetDesc` on every Map would be a COM call per buffer per frame on Archicad's
// render thread, which is the added frame cost this path is most sensitive to.
uint32_t ConstantBufferWidth (ID3D11Resource* resource);

// The Map/Unmap pair. See `ViewMatrixCandidates.hpp` for why the bytes are read
// at Unmap and never at Map.
uint32_t OnMapped (ID3D11Resource* resource, const void* mappedPointer);
uint32_t OnUnmapping (ID3D11Resource* resource);

// A bind. The `Window` form carries `firstConstant * 16`, which on this host is
// the whole address: Archicad keeps its constants in one 8 MiB ring.
void OnConstantBufferBound (uint32_t slotKind, uint32_t startSlot, ID3D11Buffer* buffer);
void OnConstantBufferBoundWindow (uint32_t slotKind, uint32_t startSlot, ID3D11Buffer* buffer,
                                  uint32_t byteOffset);

// ---- the frame clock (stage 4) ---------------------------------------------
// ⚠️ THIS IS WHAT MAKES "SAME FRAME" A MEASUREMENT RATHER THAN AN ASSUMPTION,
// and it is the whole point of stage 4. The capture is ONE DRAW STALE by
// construction: a ring is written under `Map`, unmapped, and only then bound, so
// the offset for the bytes just written is not known until after the chance to
// read them has gone, and what we copy at any Unmap is the window bound before
// it. Within a frame that is harmless -- every 3D draw in a frame shares one
// view-projection, so the previous draw's camera IS this draw's camera.
//
// ⚠️ ACROSS A FRAME BOUNDARY IT IS NOT HARMLESS, AND THAT IS EXACTLY THE ERROR
// THE OVERLAY HAS BEEN SHOWING. A camera taken from the previous frame is one
// frame stale, which at 100 fps during an orbit is precisely the ten
// milliseconds of lag the predictor was invented to paper over (PLAT-RE121).
// Stamping every captured window and every bind with the frame it happened in
// turns "is this the current frame's camera" from a hope into a comparison.
//
// Bumped by the present detour, read by the context detours, on the same render
// thread. Relaxed is enough: there is no data being ordered against it, only a
// label.
void     BeginFrame (uint64_t frameId);
uint64_t CurrentCaptureFrame ();

// The generation of the scene pass being recorded into right now. This -- not
// the Present id -- is what captured camera bindings are stamped with.
uint64_t CurrentScenePassGeneration ();

// MAIN THREAD. Fill this half of the report, and forget everything.
void FillCaptureStats (CandidateStats& stats);
void ResetCapture ();

}   // namespace viewmatrix
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv

#endif
