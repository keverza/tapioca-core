#include "DxrWorker/DxrDevice.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace evp::dxrworker {

namespace {

std::string Utf8 (const wchar_t* text)
{
    const int size = WideCharToMultiByte (CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string out (static_cast<size_t> (size - 1), '\0');
    WideCharToMultiByte (CP_UTF8, 0, text, -1, out.data (), size, nullptr, nullptr);
    return out;
}

std::string Hex (HRESULT hr)
{
    char text[16];
    std::snprintf (text, sizeof (text), "0x%08lX", static_cast<unsigned long> (hr));
    return text;
}

// NVIDIA encodes its public driver number in the UMD version's last five
// digits: 32.0.16.1088 is 610.88. Other vendors' numbering is not comparable,
// so it is reported as unknown rather than guessed.
uint32_t DriverVersion (IDXGIAdapter1* adapter, uint32_t vendorId)
{
    LARGE_INTEGER umd {};
    if (vendorId != 0x10DE || FAILED (adapter->CheckInterfaceSupport (__uuidof (IDXGIDevice), &umd)))
        return 0;
    const uint32_t build = static_cast<uint32_t> (HIWORD (umd.LowPart));
    const uint32_t revision = static_cast<uint32_t> (LOWORD (umd.LowPart));
    return (build % 10) * 10000 + revision;
}

} // namespace

const char* AdapterRefusal (uint32_t vendorId, uint32_t flags)
{
    if ((flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
        return "a software adapter";
    if (vendorId == kMicrosoftVendorId)
        return "a Microsoft software or basic adapter";
    return nullptr;
}

std::unique_ptr<DxrDevice> DxrDevice::Create (DxrCapability& capability, DxrAdapter which)
{
    capability = {};
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory2 (0, IID_PPV_ARGS (&factory));
    if (FAILED (hr)) {
        capability.message = "CreateDXGIFactory2 failed " + Hex (hr);
        return nullptr;
    }

    std::string seen;
    std::vector<ComPtr<IDXGIAdapter1>> candidates;
    if (which == DxrAdapter::WarpForDiagnosis) {
        ComPtr<IDXGIAdapter1> warp;
        if (SUCCEEDED (factory->EnumWarpAdapter (IID_PPV_ARGS (&warp))))
            candidates.push_back (warp);
    }
    else {
        ComPtr<IDXGIAdapter1> next;
        for (UINT index = 0; factory->EnumAdapters1 (index, &next) != DXGI_ERROR_NOT_FOUND; ++index)
            candidates.push_back (std::move (next));
    }
    for (const ComPtr<IDXGIAdapter1>& adapter : candidates) {
        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1 (&desc);
        const std::string name = Utf8 (desc.Description);
        const char* refusal = AdapterRefusal (desc.VendorId, desc.Flags);
        if (refusal != nullptr && which == DxrAdapter::Hardware) {
            seen += (seen.empty () ? "" : "; ") + name + ": refused, " + refusal;
            continue;
        }

        ComPtr<ID3D12Device5> device;
        hr = D3D12CreateDevice (adapter.Get (), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS (&device));
        if (FAILED (hr)) {
            seen += (seen.empty () ? "" : "; ") + name + ": no D3D12 device " + Hex (hr);
            continue;
        }
        // ⚠️ READ RaytracingTier ONLY AFTER THE QUERY SUCCEEDED: an older runtime
        // fails OPTIONS5 and leaves the struct untouched.
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 {};
        hr = device->CheckFeatureSupport (D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof (options5));
        if (FAILED (hr) || options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0) {
            seen += (seen.empty () ? "" : "; ") + name + ": no DXR tier 1.0";
            continue;
        }

        auto out = std::unique_ptr<DxrDevice> (new DxrDevice ());
        out->device_ = device;
        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device->CreateCommandQueue (&queueDesc, IID_PPV_ARGS (&out->queue_));
        if (SUCCEEDED (hr))
            hr = device->CreateCommandAllocator (D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS (&out->allocator_));
        if (SUCCEEDED (hr))
            hr = device->CreateCommandList (0, D3D12_COMMAND_LIST_TYPE_DIRECT, out->allocator_.Get (), nullptr,
                                            IID_PPV_ARGS (&out->list_));
        if (SUCCEEDED (hr))
            hr = out->list_->Close ();
        if (SUCCEEDED (hr))
            hr = device->CreateFence (0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS (&out->fence_));
        if (SUCCEEDED (hr)) {
            out->fenceEvent_ = CreateEventW (nullptr, FALSE, FALSE, nullptr);
            if (out->fenceEvent_ == nullptr)
                hr = HRESULT_FROM_WIN32 (GetLastError ());
        }
        if (FAILED (hr)) {
            seen += (seen.empty () ? "" : "; ") + name + ": queue/list setup failed " + Hex (hr);
            continue;
        }

        capability.reason = rayworker::GpuReason::None;
        capability.deviceName = name;
        capability.vendorId = desc.VendorId;
        capability.driverVersion = DriverVersion (adapter.Get (), desc.VendorId);
        capability.dedicatedMemoryBytes = desc.DedicatedVideoMemory;
        capability.raytracingTier = static_cast<uint32_t> (options5.RaytracingTier);
        return out;
    }

    capability.reason = rayworker::GpuReason::DxrUnsupported;
    capability.message = seen.empty () ? "no hardware adapter" : seen;
    return nullptr;
}

DxrDevice::~DxrDevice ()
{
    // Drain the queue before the resources it may still read are released.
    if (queue_ != nullptr && fence_ != nullptr && fenceEvent_ != nullptr) {
        ++fenceValue_;
        if (SUCCEEDED (queue_->Signal (fence_.Get (), fenceValue_)) && fence_->GetCompletedValue () < fenceValue_ &&
            SUCCEEDED (fence_->SetEventOnCompletion (fenceValue_, fenceEvent_)))
            WaitForSingleObject (fenceEvent_, 5000);
    }
    if (fenceEvent_ != nullptr)
        CloseHandle (fenceEvent_);
}

HRESULT DxrDevice::Begin ()
{
    HRESULT hr = allocator_->Reset ();
    if (SUCCEEDED (hr))
        hr = list_->Reset (allocator_.Get (), nullptr);
    return hr;
}

HRESULT DxrDevice::Submit (uint32_t timeoutMs)
{
    HRESULT hr = list_->Close ();
    if (FAILED (hr))
        return hr;
    ID3D12CommandList* lists[] = { list_.Get () };
    queue_->ExecuteCommandLists (1, lists);
    ++fenceValue_;
    hr = queue_->Signal (fence_.Get (), fenceValue_);
    if (FAILED (hr))
        return hr;
    if (fence_->GetCompletedValue () < fenceValue_) {
        hr = fence_->SetEventOnCompletion (fenceValue_, fenceEvent_);
        if (FAILED (hr))
            return hr;
        if (WaitForSingleObject (fenceEvent_, timeoutMs) != WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32 (WAIT_TIMEOUT);
    }
    // A removed device signals the fence with UINT64_MAX; the wait succeeds.
    hr = device_->GetDeviceRemovedReason ();
    if (FAILED (hr))
        return hr;
    inFlight_.clear ();
    return S_OK;
}

HRESULT DxrDevice::CreateBuffer (uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                                 D3D12_RESOURCE_FLAGS flags, ComPtr<ID3D12Resource>& out) const
{
    D3D12_HEAP_PROPERTIES heapProps {};
    heapProps.Type = heap;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes == 0 ? 4 : bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return device_->CreateCommittedResource (&heapProps, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                             IID_PPV_ARGS (&out));
}

HRESULT DxrDevice::Upload (const void* data, uint64_t bytes, D3D12_RESOURCE_STATES finalState,
                           ComPtr<ID3D12Resource>& out)
{
    ComPtr<ID3D12Resource> staging;
    HRESULT hr = CreateBuffer (bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                               D3D12_RESOURCE_FLAG_NONE, staging);
    if (FAILED (hr))
        return hr;
    void* mapped = nullptr;
    const D3D12_RANGE noRead { 0, 0 };
    hr = staging->Map (0, &noRead, &mapped);
    if (FAILED (hr))
        return hr;
    if (bytes > 0)
        std::memcpy (mapped, data, static_cast<size_t> (bytes));
    staging->Unmap (0, nullptr);

    hr = CreateBuffer (bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE, out);
    if (FAILED (hr))
        return hr;
    // Buffers decay to COMMON and promote to COPY_DEST implicitly.
    if (bytes > 0)
        list_->CopyBufferRegion (out.Get (), 0, staging.Get (), 0, bytes);
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = out.Get ();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = finalState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (bytes > 0)
        list_->ResourceBarrier (1, &barrier);
    Keep (staging);
    return S_OK;
}

rayworker::GpuReason DxrDevice::ReasonOf (HRESULT hr, const char* what, std::string& error) const
{
    using rayworker::GpuReason;
    if (hr == E_OUTOFMEMORY) {
        error = std::string (what) + ": out of memory";
        return GpuReason::OutOfMemory;
    }
    if (hr == HRESULT_FROM_WIN32 (WAIT_TIMEOUT)) {
        error = std::string (what) + ": the GPU did not finish within the deadline";
        return GpuReason::WorkerTimeout;
    }
    const HRESULT removed = device_ != nullptr ? device_->GetDeviceRemovedReason () : S_OK;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
        FAILED (removed)) {
        error = std::string (what) + ": device removed " + Hex (FAILED (removed) ? removed : hr);
        return GpuReason::DeviceRemoved;
    }
    error = std::string (what) + " failed " + Hex (hr);
    return GpuReason::WorkerFailed;
}

} // namespace evp::dxrworker
