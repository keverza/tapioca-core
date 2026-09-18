#ifndef EVP_ARCHVIZ_DXGI_INJECTEDDILIGENTCONTEXT_HPP
#define EVP_ARCHVIZ_DXGI_INJECTEDDILIGENTCONTEXT_HPP

// ArchViz/Dxgi/InjectedDiligentContext — Diligent as a GUEST on Archicad's D3D11
// device, so the overlay can be drawn by the renderer this add-on already has
// instead of by hand.
//
// ⚠️ READ private/docs/architecture/diligent/OVERLAY-INVARIANTS.md
// BEFORE EDITING A LINE OF THIS FILE, §12b in particular, and
// DILIGENT-BOUNDARY-PLAN.md beside it, which answers the four questions §12b
// says must be answered before any of this is written.
//
// ⚠️ THE BOUNDARY IS THE POINT AND CROSSING IT IS THE RISK.
// Diligent does NOT own the device, the swap chain, Present, or any state
// restoration. It draws when the injection tells it to, into the target the
// injection supplies, and everything it disturbs is put back by
// `ScopedPipelineState`. A Diligent context that creates its own device, or that
// survives a Present it did not draw in, has crossed the boundary.
//
//     KEEP RAW D3D11        Present interception, the 27-slot context hook,
//                           camera b1/b2 discovery, back-buffer discovery,
//                           state capture and EXACT restoration
//     DRAW THROUGH DILIGENT pipeline states, geometry buffers, wireframes,
//                           ghosts, heatmaps, ImGui, HarfBuzz annotations
//
// ⚠️ WHAT IT MAY HOLD, AND WHAT IT MAY NEVER HOLD.
// `AttachToD3D11Device` has no transient form: `RenderDeviceD3D11Impl` stores the
// `ID3D11Device` it is handed and `DeviceContextD3D11Impl` stores the context.
// That narrows §11's "never cache persistent raw Archicad COM pointers" to what
// it was always measured about — §11's recorded failure is `ResizeBuffers`
// refusing while a swap-chain VIEW is alive, and neither the device nor the
// immediate context is a swap-chain resource:
//
//     held for the hook's lifetime   ID3D11Device, ID3D11DeviceContext1
//     NEVER held across a resize     back buffers and every view of one
//
// So the back-buffer wrapper is keyed on the native `ID3D11Texture2D*` and
// dropped the moment that pointer differs. How often it differs is a MEASUREMENT
// this file takes (`distinctBackBuffers`), not an assumption: a flip-model chain
// rotates its buffers and a BitBlt chain does not, and the number decides whether
// the cache earns its keep.
//
// ⚠️ ATTACH IS NOT FREE AND IS THEREFORE COUNTED. It allocates a
// render device, a device context and an adapter query, which §11 forbids in a
// hot hook — so it happens ONCE, on the first composition after the backend is
// selected, and the frame it costs is reported as `attachMs` rather than left to
// be rediscovered as a stutter nobody can attribute.
//
// ⚠️ DETACH DOES NOT TOUCH ARCHICAD'S STATE, AND THAT WAS
// CHECKED. `RenderDeviceD3D11Impl::~RenderDeviceD3D11Impl` is empty and
// `DeviceContextD3D11Impl` has no destructor of its own, so releasing our
// references releases COM references and nothing else. It must stay that way: the
// viewport's teardown calls `ID3D11DeviceContext::ClearState`, which is right
// when you OWN the device and would wipe Archicad's frame here.
//
// RENDER THREAD ONLY, inside the Present detour. The Diligent immediate context
// wraps one `ID3D11DeviceContext1`, which is not free-threaded.

#include <cstdint>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext1;
struct ID3D11RenderTargetView;

namespace Diligent {
struct IDeviceContext;
struct IRenderDevice;
struct ITextureView;
} // namespace Diligent

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injecteddiligent {

// Which backend draws the overlay. The native path is not deleted when Diligent
// works -- it is the regression ORACLE, and it is deleted when Diligent has been
// correct for longer than it has.
enum class Backend : uint8_t {
    NativeD3D11 = 0, // the proven path, and the default
    Diligent = 1,    // attached and measured; what it DRAWS grows stage by stage
};

void SetBackend (Backend backend);
Backend GetBackend ();

// Attach to Archicad's device and immediate context. Idempotent: the second call
// and every later one is a cheap `true`. Returns false with `error` set, and
// records the failure, rather than throwing -- `AttachToD3D11Device` itself
// returns null pointers instead of throwing, which reads exactly like success
// until something is dereferenced.
bool Attach (ID3D11Device* device, ID3D11DeviceContext1* context, std::string& error);

// Release our references. Safe to call when not attached.
void Detach ();

bool Attached ();

Diligent::IRenderDevice* Device ();
Diligent::IDeviceContext* Context ();

// The Diligent render-target view for whatever `targetView` draws into, wrapping
// Archicad's own texture -- no copy, no second swap chain. Null if the wrap
// failed, and the reason is in `Snapshot().lastError`.
Diligent::ITextureView* WrapRenderTarget (ID3D11RenderTargetView* targetView);

// ⚠️ CALLED FROM THE ResizeBuffers DETOUR, BEFORE THE
// ORIGINAL RUNS, AND NOTHING ELSE WILL DO. `ResizeBuffers` fails outright while
// any reference to a back buffer is outstanding, and a wrapper cached across
// frames IS such a reference -- so without this, selecting the Diligent backend
// would break every window resize in Archicad, from inside Archicad's own resize
// call, surfacing as its window failing to redraw. That is §11's measured rule
// and it is the one thing the wrapper cache cannot be allowed to cost.
//
// Releases the wrappers ONLY. The device and the immediate context survive a
// resize -- they are not swap-chain resources, which is the whole distinction
// §11 turns on.
void DropWrappedTargets ();

struct Stats {
    bool attached = false;
    uint32_t attachAttempts = 0;
    uint32_t attachFailures = 0;
    // The one frame the attach cost. See the ATTACH IS NOT FREE note above.
    uint32_t attachMs = 0;
    uint32_t wraps = 0;        // wrappers created
    uint32_t wrapHits = 0;     // Presents that reused the cached wrapper
    uint32_t wrapFailures = 0; // and how often it could not be made at all
    // ⚠️ HOW OFTEN ARCHICAD'S BACK-BUFFER TEXTURE MOVED. If
    // this tracks `wraps + wrapHits` the chain rotates and a cache is pointless;
    // if it stays at one or two, the cache is the whole cost saving.
    uint32_t distinctBackBuffers = 0;
    // How many times a resize forced the wrappers to be dropped. Non-zero here
    // beside a healthy resize is the cache paying its own way.
    uint32_t wrapDropsOnResize = 0;
    char lastError[192] = {};
};
Stats Snapshot ();

// ⚠️ EVERY `Start` RESETS WHAT EVERY `Stop` LEAVES
// BEHIND (§8), AND `Detach` IS NOT ENOUGH. Detaching clears `attached` and
// leaves `wraps`, `wrapHits` and `attachMs` describing the PREVIOUS session --
// so "wrapped Archicad's back buffer" would pass on last session's evidence,
// which is the exact shape of the four faults §8 exists for.
//
// ⚠️ IT KEEPS THE BACKEND SELECTION ON PURPOSE. The
// caller chooses the backend and THEN starts; clearing the choice here would
// make the switch impossible to use.
void Reset ();

} // namespace injecteddiligent
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
