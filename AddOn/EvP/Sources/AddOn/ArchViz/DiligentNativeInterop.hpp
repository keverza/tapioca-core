#ifndef EVP_ARCHVIZ_DILIGENTNATIVEINTEROP_HPP
#define EVP_ARCHVIZ_DILIGENTNATIVEINTEROP_HPP

// ArchViz/DiligentNativeInterop — what the viewport has to do BELOW Diligent,
// for whichever backend it is running on (RE51.D2).
//
// Diligent hides the API in every ordinary draw. It does not hide these, and
// each one is a rule of the native API rather than of the engine:
//
//   teardown       D3D11's flip model defers a swap chain's destruction while the
//                  immediate context still references its back buffers (PLAT-RE39),
//                  so the NATIVE context is cleared. D3D12 has no such context
//                  state; there the queue is drained, so nothing the GPU still
//                  reads is released under it.
//   staging reads  D3D11's Map blocks until the copy is done. Diligent's D3D12
//                  backend NEVER waits when mapping a staging texture for reading
//                  (DeviceContextD3D12Impl::MapTextureSubresource says so), so a
//                  readback that relied on D3D11's wait reads whatever was there.
//   end of frame   D3D12 releases stale resources only at FinishFrame. The D3D11
//                  viewport never called it and is not changed to.
//   composition    the D3D12 composition chain is created on the COMMAND QUEUE,
//                  not the device, its buffers are indexed rather than buffer 0,
//                  and the wrapped buffer is moved to PRESENT before raw Present.
//                  RE51.D1 proved exactly this sequence live on 2026-10-31.
//
// ⚠️ D3D11 BEHAVIOUR IS UNCHANGED BY THIS FILE. Every D3D11 branch is the code
// that used to sit inline at its call site. A change to the D3D11 path here is a
// change to the production viewport and is reviewed as one.
//
// ⚠️ RENDER THREAD ONLY.

#include "ArchViz/RenderBackend.hpp"

#include <cstdint>
#include <string>

struct IDXGISwapChain1;

namespace Diligent {
struct IDeviceContext;
struct IRenderDevice;
struct ITexture;
} // namespace Diligent

namespace geomsrv::archviz {

RenderBackend BackendOfDevice (Diligent::IRenderDevice* device);
RenderBackend BackendOfContext (Diligent::IDeviceContext* context);

// Teardown: let the native API release everything the context references.
void ReleaseNativeBindings (Diligent::IDeviceContext* context);

// Call immediately before mapping a staging texture for reading. A no-op on D3D11.
void SyncBeforeStagingRead (Diligent::IDeviceContext* context);

// After the frame's Present. A no-op on D3D11.
void EndNativeFrame (Diligent::IDeviceContext* context);

// The clear A/B's native arm and readback are D3D11 calls (DiligentClearAB).
bool NativeClearABAvailable (Diligent::IDeviceContext* context);

// ---- the D3D12 composition chain ---------------------------------------------
// `desc` is a DXGI_SWAP_CHAIN_DESC1, passed opaquely so this header does not
// drag DXGI into its includers.
bool CreateCompositionChainD3D12 (Diligent::IDeviceContext* context, const void* desc, IDXGISwapChain1*& chain,
                                  std::string& error);
// The buffer about to be drawn, AddRef'd, as a raw native pointer (the key the
// target caches its wrapper on). Null on failure.
void* AcquireCompositionBufferD3D12 (IDXGISwapChain1* chain);
void ReleaseCompositionBuffer (void* buffer);
// Wraps `buffer` into a Diligent texture; null on failure.
void WrapCompositionBufferD3D12 (Diligent::IRenderDevice* device, void* buffer, Diligent::ITexture** texture);
// Moves the drawn buffer to PRESENT and submits, so the raw Present that
// follows shows a finished frame.
void PrepareCompositionPresentD3D12 (Diligent::IDeviceContext* context, Diligent::ITexture* drawn);
// ResizeBuffers refuses while anything still references a buffer, and on D3D12
// a released wrapper is only destroyed once the GPU is done with it.
void IdleBeforeCompositionResizeD3D12 (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context);

} // namespace geomsrv::archviz

#endif
