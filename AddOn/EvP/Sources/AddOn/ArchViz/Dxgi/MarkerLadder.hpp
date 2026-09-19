#ifndef EVP_ARCHVIZ_DXGI_MARKERLADDER_HPP
#define EVP_ARCHVIZ_DXGI_MARKERLADDER_HPP

// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings and
// each cost at least one. Proof primitives are OFF in production (section 10),
// composition stays at Present, and no production path may depend on a
// diagnostic having run (section 9).

// ArchViz/Dxgi/MarkerLadder -- which surface still reaches the screen, asked by
// painting a patch into each candidate and looking.
//
// ⚠️ IT EXISTS BECAUSE EVERY COUNTER WE HAVE IS BLIND TO THE
// FAULT. Under Orbit and Explore the overlay is not on screen while
// `cam=Locked`, `compose+N` on every Present, `lines=122`, every refusal counter
// zero, and camera snapshots, decodes and adoptions all moving 1:1. Six stages
// of camera instrumentation agreed the pipeline was healthy and the screen
// disagreed. That means the counters describe a surface which is not the one
// being displayed, and no further counter can say which -- only a pixel can.
//
// ⚠️ SO IT IS A LADDER AND NOT A MARKER. One square proves
// "something of ours can reach the screen" and nothing else. Several, at points
// along the path from the model draw to Present, each in its own slot down the
// left edge and its own colour, turn one run into a bracket:
//
//     A  after the recognised model draw          scene colour target
//     E  immediately before Present               the back buffer
//
// ⚠️ B AND C EXISTED, REPORTED, AND WERE REMOVED. They
// painted at the scene boundary from inside `DetourOMSetRenderTargets`, which
// meant calling `OMGetRenderTargets`, `QueryInterface` and `ClearView` on
// Archicad's own context from inside a detour on that context, before the
// original call was forwarded and without `ScopedInjectionGuard`. Archicad
// crashed on a Floor Plan -> 3D transition with the ladder armed
// (2026-09-19 17:34), immediately after the context hook installed.
//
// Their answer is kept because it does not need repeating: B was visible in
// ALL THREE modes and C in NONE. Archicad's scene colour target reaches the
// screen; the target it switches to at the boundary does not.
//
// A disappears while E survives  -> what we put in Archicad's scene target does
//                                   not reach the screen in that mode, so drawing
//                                   into it is the wrong architecture
// A survives                     -> the scene pair IS carried through, and the
//                                   preferred architecture -- draw into
//                                   Archicad's own colour and depth before its
//                                   composite -- works
// E disappears                   -> another presentation path entirely
//
// ⚠️ D IS DELIBERATELY ABSENT. "After the composite" needs a hook
// inside the draw detours, which are the hottest path in the tree and the one
// place a diagnostic must not add work -- the same reason B and C were removed.
//
// ⚠️ AND IT PAINTS WITH `ClearView`, NOT WITH A DRAW. That is
// the whole reason this is cheap enough to exist. `ID3D11DeviceContext1::
// ClearView` takes the VIEW and a rectangle; it binds nothing, needs no shader,
// no vertex buffer, no input layout and no depth state, and it leaves the
// pipeline exactly as it found it. Section 11 demands full state restoration for
// anything that draws -- this never disturbs the state, so there is nothing to
// restore, which is a stronger guarantee than restoring correctly.
//
// ⚠️ OFF BY DEFAULT AND OFF AT EVERY ARM (section 10). A
// square painted into Archicad's frame is a defect to anyone who did not ask for
// it, and a run that inherits the previous one's enable is a run whose evidence
// nobody can trust.
//
// THREAD SAFETY. `Paint` and `PaintSwapChain` run on Archicad's render thread
// inside a detour: atomics only, no lock, no allocation, no ACAPI. Everything
// else is main thread.

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct IDXGISwapChain;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace markerladder {

// ⚠️ THE NAMES AND THE ORDER ARE KEPT THOUGH B AND C NO
// LONGER PAINT. Their slots stay reserved so a reader comparing an old log
// against a new one is not silently reading different rungs under the same
// letters, and so the counts in a stored report still mean what they said.
enum class Rung : uint32_t {
    AfterModelDraw = 0, // A -- red
    SceneBoundary,      // B -- yellow, RETIRED, see above
    NextTarget,         // C -- green, RETIRED, see above
    BeforePresent,      // E -- magenta
    Count
};

const char* RungName (Rung rung);

// MAIN THREAD. Off by default; `Reset` is called at every arm.
void SetEnabled (bool enabled);
bool Enabled ();

// RENDER THREAD. Paint this rung's patch into `view`.
//
// ⚠️ THE VIEW IS NOT REQUIRED TO BE BOUND. `ClearView`
// addresses the view itself. That is what made the retired B and C possible at
// all -- and, separately, what made them unsafe: a clear issued while the driver
// was part way through a render-target transition.
void Paint (ID3D11DeviceContext* context, ID3D11RenderTargetView* view, Rung rung);

// RENDER THREAD. Paint into the swap chain's back buffer -- rung E, which has a
// chain rather than a view.
void PaintSwapChain (IDXGISwapChain* swapChain, Rung rung);

// RENDER THREAD. The render target currently bound, AddRef'd -- the caller
// releases. Returns nullptr when nothing is bound.
//
// ⚠️ IT IS A COM CALL IN A DETOUR AND IT IS GATED ON
// `Enabled ()` BY EVERY CALLER. `OMGetRenderTargets` is cheap and does not
// block, but it is not free, and a diagnostic that costs anything while switched
// off is a diagnostic that changed the thing it was built to measure.
ID3D11RenderTargetView* BoundTarget (ID3D11DeviceContext* context);

struct Stats {
    uint64_t painted[size_t (Rung::Count)] = {};
    uint64_t failures = 0;
};
Stats GetStats ();

// MAIN THREAD, at every arm and every teardown.
void Reset ();

} // namespace markerladder
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
