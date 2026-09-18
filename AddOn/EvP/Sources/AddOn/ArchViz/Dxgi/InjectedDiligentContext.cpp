// See OVERLAY-INVARIANTS.md §12b and DILIGENT-BOUNDARY-PLAN.md. The header
// carries the reasoning; this file carries the calls.

#include "ArchViz/Dxgi/InjectedDiligentContext.hpp"

#include "ArchViz/ArchVizLog.hpp"

#include <windows.h>
#include <d3d11_1.h> // Must precede Diligent's D3D11 interop headers (Probe 1a).
#include <EngineFactoryD3D11.h>
#include <RefCntAutoPtr.hpp>
#include <RenderDeviceD3D11.h>

#include <chrono>
#include <cstring>
#include <unordered_map>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injecteddiligent {

namespace {

struct Wrapped {
    Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
    Diligent::ITextureView* view = nullptr; // owned by `texture`
};

struct State {
    Backend backend = Backend::NativeD3D11;
    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device;
    Diligent::RefCntAutoPtr<Diligent::IRenderDeviceD3D11> deviceD3D11;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context;
    // ⚠️ ONE ENTRY IN PRACTICE, A MAP BECAUSE THAT IS UNPROVEN.
    // A BitBlt chain hands back the same texture every Present and a flip-model
    // chain rotates two or three. `distinctBackBuffers` is what says which this
    // is; until it has said so, a map costs one hash and assumes nothing.
    std::unordered_map<ID3D11Texture2D*, Wrapped> wrapped;
    Stats stats;
};

State& Get ()
{
    static State state;
    return state;
}

void SetError (const char* text)
{
    State& s = Get ();
    std::snprintf (s.stats.lastError, sizeof (s.stats.lastError), "%s", text != nullptr ? text : "");
}

} // namespace

void SetBackend (Backend backend)
{
    Get ().backend = backend;
}

Backend GetBackend ()
{
    return Get ().backend;
}

bool Attached ()
{
    return Get ().stats.attached;
}

Diligent::IRenderDevice* Device ()
{
    return Get ().device.RawPtr ();
}

Diligent::IDeviceContext* Context ()
{
    return Get ().context.RawPtr ();
}

bool Attach (ID3D11Device* device, ID3D11DeviceContext1* context, std::string& error)
{
    State& s = Get ();
    if (s.stats.attached)
        return true;

    if (device == nullptr || context == nullptr) {
        error = "no Archicad device or immediate context to attach to";
        SetError (error.c_str ());
        ++s.stats.attachFailures;
        return false;
    }

    ++s.stats.attachAttempts;
    const auto started = std::chrono::steady_clock::now ();

    Diligent::EngineD3D11CreateInfo engineCI;
    // ⚠️ NO DEFERRED CONTEXTS. A deferred context records a
    // command list and `FinishCommandList` puts the context back to its DEFAULT
    // state as if `ClearState` had been called -- on Archicad's context, mid
    // frame. Diligent even documents that at DeviceContextD3D11Impl.cpp:1955.
    engineCI.NumDeferredContexts = 0;

    Diligent::IEngineFactoryD3D11* factory = Diligent::GetEngineFactoryD3D11 ();
    if (factory == nullptr) {
        error = "GetEngineFactoryD3D11 returned nothing";
        SetError (error.c_str ());
        ++s.stats.attachFailures;
        return false;
    }

    // ⚠️ IT RETURNS NULL RATHER THAN THROWING, AND THE NULL IS
    // THE ONLY SIGNAL. EngineFactoryD3D11.cpp:412 logs and releases on every
    // failure path -- a version mismatch, an adapter query that failed, a context
    // with no ID3D11DeviceContext1 -- and returns normally. Not testing both
    // pointers here would read as success right up to the first dereference.
    Diligent::IRenderDevice* rawDevice = nullptr;
    Diligent::IDeviceContext* rawContext = nullptr;
    factory->AttachToD3D11Device (device, context, engineCI, &rawDevice, &rawContext);
    s.device.Attach (rawDevice);
    s.context.Attach (rawContext);

    if (s.device == nullptr || s.context == nullptr) {
        s.device.Release ();
        s.context.Release ();
        error = "AttachToD3D11Device returned no render device or immediate context";
        SetError (error.c_str ());
        ++s.stats.attachFailures;
        ArchVizLog ("injected Diligent: attach REFUSED -- " + error);
        return false;
    }

    s.deviceD3D11 = Diligent::RefCntAutoPtr<Diligent::IRenderDeviceD3D11> { s.device, Diligent::IID_RenderDeviceD3D11 };
    if (s.deviceD3D11 == nullptr) {
        Detach ();
        error = "the attached device does not expose IRenderDeviceD3D11, so no native texture can be wrapped";
        SetError (error.c_str ());
        ++s.stats.attachFailures;
        return false;
    }

    const auto elapsed = std::chrono::steady_clock::now () - started;
    s.stats.attachMs = uint32_t (std::chrono::duration_cast<std::chrono::milliseconds> (elapsed).count ());
    s.stats.attached = true;
    s.stats.lastError[0] = 0;
    ArchVizLog ("injected Diligent: ATTACHED to Archicad's own device and immediate context in " +
                std::to_string (s.stats.attachMs) + " ms -- no second device, no second swap chain");
    return true;
}

void Detach ()
{
    State& s = Get ();
    // ⚠️ THE WRAPPERS GO FIRST AND THEY GO UNCONDITIONALLY.
    // Each holds a reference to an Archicad back buffer, and §11's measured rule
    // is that `ResizeBuffers` fails while a swap-chain view is alive.
    s.wrapped.clear ();
    s.deviceD3D11.Release ();
    // Releasing is all that happens. `RenderDeviceD3D11Impl::~RenderDeviceD3D11Impl`
    // is empty and `DeviceContextD3D11Impl` has no destructor of its own, so
    // nothing here reaches Archicad's context -- see the header. Do NOT add a
    // `ClearState` in sympathy with the viewport's teardown: the viewport owns
    // its device and we are a guest on somebody's live frame.
    s.context.Release ();
    s.device.Release ();
    if (s.stats.attached)
        ArchVizLog ("injected Diligent: detached, Archicad's device untouched");
    s.stats.attached = false;
}

Diligent::ITextureView* WrapRenderTarget (ID3D11RenderTargetView* targetView)
{
    State& s = Get ();
    if (!s.stats.attached || targetView == nullptr)
        return nullptr;

    ID3D11Resource* resource = nullptr;
    targetView->GetResource (&resource);
    if (resource == nullptr) {
        ++s.stats.wrapFailures;
        SetError ("the composition render target has no resource behind it");
        return nullptr;
    }
    ID3D11Texture2D* texture = nullptr;
    const HRESULT hr = resource->QueryInterface (__uuidof (ID3D11Texture2D), reinterpret_cast<void**> (&texture));
    resource->Release ();
    if (FAILED (hr) || texture == nullptr) {
        ++s.stats.wrapFailures;
        SetError ("the composition render target is not an ID3D11Texture2D");
        return nullptr;
    }

    auto it = s.wrapped.find (texture);
    if (it != s.wrapped.end ()) {
        ++s.stats.wrapHits;
        texture->Release ();
        return it->second.view;
    }

    ++s.stats.distinctBackBuffers;
    Wrapped entry;
    // RESOURCE_STATE_RENDER_TARGET, not UNKNOWN: we know what it is, and saying
    // so spares Diligent a transition it would otherwise guess at. Same call, and
    // the same reasoning, as DiligentViewportTarget's composition path -- which is
    // what Probe 1b proved band-for-band against raw D3D11.
    s.deviceD3D11->CreateTexture2DFromD3DResource (texture, Diligent::RESOURCE_STATE_RENDER_TARGET, &entry.texture);
    texture->Release ();
    if (entry.texture == nullptr) {
        ++s.stats.wrapFailures;
        SetError ("CreateTexture2DFromD3DResource refused Archicad's back buffer");
        return nullptr;
    }
    entry.view = entry.texture->GetDefaultView (Diligent::TEXTURE_VIEW_RENDER_TARGET);
    if (entry.view == nullptr) {
        ++s.stats.wrapFailures;
        SetError ("the wrapped back buffer has no default render-target view");
        return nullptr;
    }
    ++s.stats.wraps;
    s.wrapped.emplace (texture, std::move (entry));
    return s.wrapped[texture].view;
}

void DropWrappedTargets ()
{
    State& s = Get ();
    if (s.wrapped.empty ())
        return;
    s.wrapped.clear ();
    ++s.stats.wrapDropsOnResize;
}

Stats Snapshot ()
{
    return Get ().stats;
}

void Reset ()
{
    Detach ();
    State& s = Get ();
    s.stats = Stats {};
    s.backend = Backend::NativeD3D11;
}

} // namespace injecteddiligent
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
