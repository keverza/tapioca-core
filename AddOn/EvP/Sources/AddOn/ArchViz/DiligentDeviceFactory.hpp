#ifndef EVP_ARCHVIZ_DILIGENTDEVICEFACTORY_HPP
#define EVP_ARCHVIZ_DILIGENTDEVICEFACTORY_HPP

// ArchViz/DiligentDeviceFactory — creates the viewport's device on the backend
// RenderBackend chose, and falls back to D3D11 whenever D3D12 does not come up
// exactly as chosen (RE51.D2).
//
// ⚠️ THE ADAPTER IS NAMED, NOT LEFT TO THE ENGINE. Left to itself Diligent takes
// the first hardware adapter it can create a device on and, failing that,
// quietly hands back WARP -- which is what the first feasibility run recorded.
// Here the adapters are enumerated, the rule picks one by index, the device is
// created on that index, and the device that came back is checked again
// (RefuseCreatedD3D12Device) before anything is drawn with it.
//
// ⚠️ A D3D12 FAILURE NEVER FAILS THE VIEWPORT. It is logged with its reason and
// the D3D11 device is created exactly as it always was. Only a D3D11 failure is
// a failure.
//
// ⚠️ RENDER THREAD ONLY: the device and immediate context belong to the thread
// that will draw with them.

#include "ArchViz/RenderBackend.hpp"

#include <RefCntAutoPtr.hpp>

#include <string>

namespace Diligent {
struct IDeviceContext;
struct IRenderDevice;
} // namespace Diligent

namespace geomsrv::archviz {

// What this start forces, whatever the preference: the host-composite mirror
// shares a D3D11 texture with the injection on Archicad's own device, so an
// overlay surface that feeds it stays D3D11.
RenderBackendConstraints ViewportBackendConstraints (bool overlaySurface);

// On false `error` says why even D3D11 could not be created; `choice` always
// says which backend was used and why.
bool CreateViewportDevice (RenderBackendPreference preference, const RenderBackendConstraints& constraints,
                           Diligent::RefCntAutoPtr<Diligent::IRenderDevice>& device,
                           Diligent::RefCntAutoPtr<Diligent::IDeviceContext>& context, RenderBackendChoice& choice,
                           std::string& error);

// The D3D12 device has been created but something built on it (the swap chain,
// the composition chain) would not come up: release both and create D3D11.
// `why` is appended to `choice.reason`.
bool FallBackToD3D11 (const std::string& why, Diligent::RefCntAutoPtr<Diligent::IRenderDevice>& device,
                      Diligent::RefCntAutoPtr<Diligent::IDeviceContext>& context, RenderBackendChoice& choice,
                      std::string& error);

} // namespace geomsrv::archviz

#endif
