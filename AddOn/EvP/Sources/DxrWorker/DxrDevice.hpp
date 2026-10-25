#ifndef EVP_DXRWORKER_DXRDEVICE_HPP
#define EVP_DXRWORKER_DXRDEVICE_HPP

// DxrWorker/DxrDevice -- the worker's own D3D12 device: adapter choice, the
// DXR capability check, one direct queue, one command list and a fence.
//
// ⚠️ THIS RUNS IN THE WORKER PROCESS, NEVER INSIDE ARCHICAD. The add-on's
// renderer owns a D3D11 device; an analysis device beside it would share the
// driver's process state and a device reset would take the viewport with it.
// The worker is a separate executable for exactly that reason (G1/G2).
//
// Submission is synchronous: record, Submit (close, execute, wait on the fence
// with a deadline). A failed wait reports the device-removed reason rather
// than the generic HRESULT, so the add-on's notice names what happened.

#include "RayWorker/RayWorkerReason.hpp"

// <d3d12.h> pulls in <windows.h>, whose min/max macros break std::min and
// numeric_limits<>::max. Include this header first in a translation unit.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace evp::dxrworker {

using Microsoft::WRL::ComPtr;

// What the adapter scan found; `reason` None when DXR Tier >= 1.0 is usable.
struct DxrCapability {
    rayworker::GpuReason reason = rayworker::GpuReason::GpuUnsupported;
    std::string message;
    std::string deviceName; // UTF-8
    uint32_t vendorId = 0;
    uint32_t driverVersion = 0; // NVIDIA style, e.g. 61088 for 610.88; 0 when unknown
    uint64_t dedicatedMemoryBytes = 0;
    uint32_t raytracingTier = 0; // D3D12_RAYTRACING_TIER value
};

class DxrDevice final {
  public:
    // The first hardware adapter whose device reports RaytracingTier >= 1.0.
    // Software adapters are skipped. Null with `capability` explaining why.
    static std::unique_ptr<DxrDevice> Create (DxrCapability& capability);
    ~DxrDevice ();
    DxrDevice (const DxrDevice&) = delete;
    DxrDevice& operator= (const DxrDevice&) = delete;

    ID3D12Device5* Device () const
    {
        return device_.Get ();
    }
    ID3D12GraphicsCommandList4* List () const
    {
        return list_.Get ();
    }

    // Reset the allocator and list for recording.
    HRESULT Begin ();
    // Close, execute and wait. Staging resources kept by Keep() are released
    // once the GPU is done with them.
    HRESULT Submit (uint32_t timeoutMs);
    // Hold a resource until the next Submit completes.
    void Keep (ComPtr<ID3D12Resource> resource)
    {
        inFlight_.push_back (std::move (resource));
    }

    HRESULT CreateBuffer (uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags,
                          ComPtr<ID3D12Resource>& out) const;
    // A default-heap buffer holding `bytes` of `data`, copied through an upload
    // buffer recorded on the open list; ends in `finalState`.
    HRESULT Upload (const void* data, uint64_t bytes, D3D12_RESOURCE_STATES finalState, ComPtr<ID3D12Resource>& out);

    // The stable reason and a message for a failed HRESULT (device removal is
    // resolved to its cause).
    rayworker::GpuReason ReasonOf (HRESULT hr, const char* what, std::string& error) const;

  private:
    DxrDevice () = default;

    ComPtr<ID3D12Device5> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList4> list_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    uint64_t fenceValue_ = 0;
    std::vector<ComPtr<ID3D12Resource>> inFlight_;
};

} // namespace evp::dxrworker

#endif
