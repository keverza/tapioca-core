// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings and each
// cost at least one. The guest draws at Present inside ScopedPipelineState, holds
// Archicad's device and context and never one of its resources (§11, §12b).
// ArchViz/Dxgi/DiligentGuest -- see the header.

#include "ArchViz/Dxgi/DiligentGuest.hpp"

#include "ArchViz/ArchVizLog.hpp"

#include <windows.h>
#include <d3d11_1.h> // Must precede Diligent's D3D11 interop headers (Probe 1a).
#include <EngineFactoryD3D11.h>
#include <RefCntAutoPtr.hpp>
#include <RenderDeviceD3D11.h>

#include <chrono>
#include <cstdio>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {

struct DiligentGuest::Impl {
    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context;
    ID3D11Device* native = nullptr; // identity only: which device the wrappers are for
    Stats stats;

    void Fail (const std::string& text)
    {
        std::snprintf (stats.lastError, sizeof (stats.lastError), "%s", text.c_str ());
        ++stats.attachFailures;
    }
};

DiligentGuest::DiligentGuest () : impl_ (new Impl ())
{
}

DiligentGuest::~DiligentGuest ()
{
    Detach ();
    delete impl_;
}

bool DiligentGuest::Attach (ID3D11Device* device, ID3D11DeviceContext* context, std::string& error)
{
    Impl& s = *impl_;
    if (s.stats.attached && device == s.native)
        return true;
    if (s.stats.attached)
        Detach ();
    if (device == nullptr || context == nullptr) {
        error = "no Archicad device or immediate context to attach to";
        s.Fail (error);
        return false;
    }
    // ⚠️ THE D3D11.1 CONTEXT IS A PRECONDITION, TESTED HERE: the attach throws
    // without it (EngineFactoryD3D11.cpp:384), and the guard needs it anyway.
    ID3D11DeviceContext1* context1 = nullptr;
    if (FAILED (context->QueryInterface (__uuidof (ID3D11DeviceContext1), reinterpret_cast<void**> (&context1))) ||
        context1 == nullptr) {
        error = "Archicad's immediate context has no ID3D11DeviceContext1";
        s.Fail (error);
        return false;
    }
    const auto started = std::chrono::steady_clock::now ();
    Diligent::EngineD3D11CreateInfo engine;
    // ⚠️ NO DEFERRED CONTEXTS: FinishCommandList resets a context to its defaults,
    // which on Archicad's would be the middle of its frame.
    engine.NumDeferredContexts = 0;
    Diligent::IEngineFactoryD3D11* factory = Diligent::GetEngineFactoryD3D11 ();
    Diligent::IRenderDevice* rawDevice = nullptr;
    Diligent::IDeviceContext* rawContext = nullptr;
    if (factory != nullptr)
        factory->AttachToD3D11Device (device, context1, engine, &rawDevice, &rawContext);
    context1->Release ();
    s.device.Attach (rawDevice);
    s.context.Attach (rawContext);
    // ⚠️ NULL IS THE ONLY FAILURE SIGNAL (EngineFactoryD3D11.cpp:412).
    if (s.device == nullptr || s.context == nullptr) {
        s.device.Release ();
        s.context.Release ();
        error = factory == nullptr ? "GetEngineFactoryD3D11 returned nothing"
                                   : "AttachToD3D11Device returned no device or no context";
        s.Fail (error);
        return false;
    }
    s.native = device;
    s.stats.attached = true;
    ++s.stats.attaches;
    s.stats.lastError[0] = 0;
    // The adapter's name, once per attach and off the hot path: the HUD's Debug page says it.
    strncpy_s (s.stats.adapter, sizeof (s.stats.adapter), s.device->GetAdapterInfo ().Description, _TRUNCATE);
    s.stats.attachMilliseconds = uint32_t (
        std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now () - started).count ());
    return true;
}

void DiligentGuest::Detach ()
{
    Impl& s = *impl_;
    // Releasing is all that happens; nothing here reaches Archicad's context.
    s.context.Release ();
    s.device.Release ();
    s.native = nullptr;
    s.stats.attached = false;
}

bool DiligentGuest::Attached () const
{
    return impl_->stats.attached;
}

bool DiligentGuest::DeviceChanged (ID3D11Device* device) const
{
    return impl_->stats.attached && device != impl_->native;
}

ID3D11Device* DiligentGuest::NativeDevice () const
{
    return impl_->native;
}

Diligent::IRenderDevice* DiligentGuest::Device () const
{
    return impl_->device.RawPtr ();
}

Diligent::IDeviceContext* DiligentGuest::Context () const
{
    return impl_->context.RawPtr ();
}

DiligentGuest::Stats DiligentGuest::GetStats () const
{
    return impl_->stats;
}

void DiligentGuest::BeginDraw (ID3D11DeviceContext* native, ID3D11RenderTargetView* target,
                               ID3D11DepthStencilView* depth)
{
    if (impl_->context == nullptr || native == nullptr)
        return;
    impl_->context->InvalidateState ();
    native->OMSetRenderTargets (1, &target, depth);
}

} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
