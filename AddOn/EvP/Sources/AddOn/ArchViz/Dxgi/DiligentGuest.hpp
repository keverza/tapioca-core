#ifndef EVP_ARCHVIZ_DXGI_DILIGENTGUEST_HPP
#define EVP_ARCHVIZ_DXGI_DILIGENTGUEST_HPP

// ArchViz/Dxgi/DiligentGuest -- one overlay's Diligent device and immediate context,
// attached to Archicad's own D3D11 device and context (§12b): the renderer that
// draws the overlays' text, dimensions, styled meshes, heatmaps and legends.
//
// ⚠️ READ OVERLAY-INVARIANTS.md §11 AND §12b, AND DILIGENT-BOUNDARY-PLAN.md.
// `AttachToD3D11Device` is the only route, it returns null rather than throwing,
// and it allocates -- so it runs once, off every hot path, and its cost is counted.
//
// ⚠️ ONE PER OVERLAY, NOT ONE FOR BOTH (§12). The floor plan draws on the main
// thread at its Present and the 3D window on Archicad's render thread at its; a
// Diligent immediate context has one native context's thread affinity, and a 3D
// failure must never be able to take the plan's renderer with it. Two attachments to
// one device cost two small wrappers; sharing one would cost the independence.
//
// ⚠️ IT HOLDS THE DEVICE AND THE CONTEXT, NEVER A RESOURCE OF ARCHICAD'S. §12b
// narrowed §11's first rule to exactly that. The guest never wraps a back buffer:
// the caller binds Archicad's render target NATIVELY after `InvalidateState`
// (`BeginDraw`), and Diligent's D3D11 context never writes OM render targets outside
// SetRenderTargets (DeviceContextD3D11Impl.cpp: CommitRenderTargets, ResetRenderTargets
// and InvalidateState are the only writers; PrepareForDraw's check of them compiles
// only under DILIGENT_DEVELOPMENT, which the add-on's RelWithDebInfo does not define).
// So `ResizeBuffers` can never be refused on our account.
//
// ⚠️ DETACH TOUCHES NOTHING OF ARCHICAD'S: releasing a Diligent device releases COM
// references and nothing else (InjectedDiligentContext.hpp checked the destructors).
// Never `ClearState` here -- that is right only for a device you own.
//
// THREAD: whichever thread the owning overlay draws on, and only that one.

#include <cstdint>
#include <string>

struct ID3D11DepthStencilView;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11DeviceContext1;
struct ID3D11RenderTargetView;

namespace Diligent {
struct IDeviceContext;
struct IRenderDevice;
} // namespace Diligent

namespace geomsrv {
namespace archviz {
namespace dxgi {

class DiligentGuest final {
  public:
    struct Stats {
        bool attached = false;
        uint32_t attaches = 0;
        uint32_t attachFailures = 0;
        uint32_t attachMilliseconds = 0; // the one frame the attach cost
        char lastError[192] = {};
    };

    DiligentGuest ();
    ~DiligentGuest ();
    DiligentGuest (const DiligentGuest&) = delete;
    DiligentGuest& operator= (const DiligentGuest&) = delete;

    // Idempotent for the same device. A different device detaches first: every
    // object the caller built on the old one must already be released, which is why
    // `DeviceChanged` exists.
    bool Attach (ID3D11Device* device, ID3D11DeviceContext* context, std::string& error);
    void Detach ();

    bool Attached () const;
    bool DeviceChanged (ID3D11Device* device) const;
    ID3D11Device* NativeDevice () const;

    Diligent::IRenderDevice* Device () const;
    Diligent::IDeviceContext* Context () const;

    Stats GetStats () const;

    // ⚠️ INSIDE THE CALLER'S ScopedPipelineState, ALWAYS. `InvalidateState` writes
    // null to six shader stages, the render target, the vertex buffers, the input
    // layout and the index buffer ON THE NATIVE CONTEXT (DeviceContextD3D11Impl.cpp
    // :2155) -- it has to run, since Archicad rebound everything underneath Diligent's
    // cache, and it is safe only inside the guard. Then the caller's target is bound
    // natively. The viewport is left as the caller set it.
    void BeginDraw (ID3D11DeviceContext* native, ID3D11RenderTargetView* target, ID3D11DepthStencilView* depth);

  private:
    struct Impl;
    Impl* impl_;
};

} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
