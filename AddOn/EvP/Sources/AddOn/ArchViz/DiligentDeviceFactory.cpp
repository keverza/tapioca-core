#include "ArchViz/DiligentDeviceFactory.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/DiligentNativeInterop.hpp"
#include "ArchViz/Dxgi/HostComposite.hpp"

#include <windows.h>
#include <d3d11.h> // Must precede any Diligent D3D11 interop header (Probe 1a).
#include <d3d12.h>

#include <EngineFactoryD3D11.h>
#include <EngineFactoryD3D12.h>
#include <RenderDevice.h>

#include <vector>

namespace geomsrv::archviz {
namespace {

using Diligent::RefCntAutoPtr;

// Direct3D 12 refuses anything below 11.0; the enumeration and the creation
// must use the same minimum or the adapter indices do not line up.
const Diligent::Version kMinD3D12FeatureLevel { 11, 0 };

RenderAdapterKind KindOf (Diligent::ADAPTER_TYPE type)
{
    switch (type) {
        case Diligent::ADAPTER_TYPE_SOFTWARE:
            return RenderAdapterKind::Software;
        case Diligent::ADAPTER_TYPE_INTEGRATED:
            return RenderAdapterKind::Integrated;
        case Diligent::ADAPTER_TYPE_DISCRETE:
            return RenderAdapterKind::Discrete;
        default:
            return RenderAdapterKind::Unknown;
    }
}

RenderAdapterFacts FactsOf (const Diligent::GraphicsAdapterInfo& info, bool rayTracingEnabled)
{
    RenderAdapterFacts facts;
    facts.name = info.Description;
    facts.vendorId = info.VendorId;
    facts.kind = KindOf (info.Type);
    facts.rayTracingEnabled = rayTracingEnabled;
    facts.rayTracingCaps = uint32_t (info.RayTracing.CapFlags);
    return facts;
}

bool CreateD3D11 (RefCntAutoPtr<Diligent::IRenderDevice>& device, RefCntAutoPtr<Diligent::IDeviceContext>& context,
                  std::string& error)
{
    // Exactly the device the viewport has always created.
    Diligent::EngineD3D11CreateInfo engineCI;
    engineCI.Features.Tessellation = Diligent::DEVICE_FEATURE_STATE_OPTIONAL;
    engineCI.Features.GeometryShaders = Diligent::DEVICE_FEATURE_STATE_OPTIONAL;
    Diligent::GetEngineFactoryD3D11 ()->CreateDeviceAndContextsD3D11 (engineCI, &device, &context);
    if (device == nullptr || context == nullptr) {
        error = "Diligent returned no D3D11 device or immediate context";
        return false;
    }
    return true;
}

// The D3D12 attempt. On false `why` is the reason and nothing is left behind.
bool TryCreateD3D12 (RenderBackendChoice& choice, RefCntAutoPtr<Diligent::IRenderDevice>& device,
                     RefCntAutoPtr<Diligent::IDeviceContext>& context, std::string& why)
{
    Diligent::EngineD3D12CreateInfo engineCI;
    engineCI.GraphicsAPIVersion = kMinD3D12FeatureLevel;
    engineCI.AdapterId = static_cast<Diligent::Uint32> (choice.adapterIndex);
    engineCI.Features.Tessellation = Diligent::DEVICE_FEATURE_STATE_OPTIONAL;
    engineCI.Features.GeometryShaders = Diligent::DEVICE_FEATURE_STATE_OPTIONAL;
    // REQUIRED, not optional: the adapter was chosen for it, so a device without
    // it is the wrong device rather than a lesser one.
    engineCI.Features.RayTracing = Diligent::DEVICE_FEATURE_STATE_ENABLED;
    Diligent::GetEngineFactoryD3D12 ()->CreateDeviceAndContextsD3D12 (engineCI, &device, &context);
    if (device == nullptr || context == nullptr) {
        device.Release ();
        context.Release ();
        why = "CreateDeviceAndContextsD3D12 failed on adapter " + std::to_string (choice.adapterIndex) +
              " (see the Diligent lines above)";
        return false;
    }
    CreatedDeviceFacts created;
    created.adapter = FactsOf (device->GetAdapterInfo (),
                               device->GetDeviceInfo ().Features.RayTracing == Diligent::DEVICE_FEATURE_STATE_ENABLED);
    created.ndcMinZ = device->GetDeviceInfo ().NDC.MinZ;
    why = RefuseCreatedD3D12Device (choice, created);
    if (!why.empty ()) {
        context->WaitForIdle ();
        context.Release ();
        device.Release ();
        return false;
    }
    return true;
}

} // namespace

RenderBackendConstraints ViewportBackendConstraints (bool overlaySurface)
{
    RenderBackendConstraints constraints;
    if (overlaySurface && dxgi::HostCompositeEnabled ()) {
        constraints.requiresD3D11Interop = true;
        constraints.interopReason = "the host-composite mirror";
    }
    return constraints;
}

bool CreateViewportDevice (RenderBackendPreference preference, const RenderBackendConstraints& constraints,
                           RefCntAutoPtr<Diligent::IRenderDevice>& device,
                           RefCntAutoPtr<Diligent::IDeviceContext>& context, RenderBackendChoice& choice,
                           std::string& error)
{
    std::vector<RenderAdapterFacts> adapters;
    RenderBackendConstraints effective = constraints;
    if (preference == RenderBackendPreference::Auto && !constraints.requiresD3D11Interop) {
        Diligent::IEngineFactoryD3D12* factory = Diligent::GetEngineFactoryD3D12 ();
        // A machine without the D3D12 runtime is an ordinary D3D11 machine.
        if (factory == nullptr || !factory->LoadD3D12 ("d3d12.dll")) {
            effective.d3d12Unavailable = true;
        }
        else {
            Diligent::Uint32 count = 0;
            factory->EnumerateAdapters (kMinD3D12FeatureLevel, count, nullptr);
            std::vector<Diligent::GraphicsAdapterInfo> infos (count);
            if (count > 0)
                factory->EnumerateAdapters (kMinD3D12FeatureLevel, count, infos.data ());
            for (Diligent::Uint32 index = 0; index < count; ++index) {
                const Diligent::GraphicsAdapterInfo& info = infos[index];
                adapters.push_back (
                    FactsOf (info, info.Features.RayTracing != Diligent::DEVICE_FEATURE_STATE_DISABLED));
            }
        }
    }

    choice = ChooseRenderBackend (preference, adapters, effective);
    if (choice.backend == RenderBackend::D3D12) {
        std::string why;
        if (TryCreateD3D12 (choice, device, context, why)) {
            ArchVizLog ("[RE51.D2] backend D3D12: " + choice.reason);
            return true;
        }
        choice = RenderBackendChoice {};
        choice.reason = "auto: D3D12 fell back to D3D11 -- " + why;
    }
    ArchVizLog ("[RE51.D2] backend D3D11: " + choice.reason);
    if (!CreateD3D11 (device, context, error))
        return false;
    choice.adapterName = device->GetAdapterInfo ().Description; // the D3D11 device's own
    return true;
}

bool FallBackToD3D11 (const std::string& why, RefCntAutoPtr<Diligent::IRenderDevice>& device,
                      RefCntAutoPtr<Diligent::IDeviceContext>& context, RenderBackendChoice& choice, std::string& error)
{
    ReleaseNativeBindings (context);
    context.Release ();
    device.Release ();
    choice = RenderBackendChoice {};
    choice.reason = "auto: D3D12 fell back to D3D11 -- " + why;
    ArchVizLog ("[RE51.D2] backend D3D11: " + choice.reason);
    if (!CreateD3D11 (device, context, error))
        return false;
    choice.adapterName = device->GetAdapterInfo ().Description; // the D3D11 device's own
    return true;
}

} // namespace geomsrv::archviz
