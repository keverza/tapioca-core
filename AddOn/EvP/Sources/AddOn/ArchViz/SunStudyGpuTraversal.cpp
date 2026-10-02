#include "ArchViz/SunStudyGpuTraversal.hpp"
#include "ArchViz/SunStudyGpuShader.hpp"
#include "ArchViz/ArchVizLog.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

namespace geomsrv::archviz {

namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
constexpr uint32_t kPacketRays = 4096;
constexpr uint32_t kGroupRays = 64;
constexpr uint32_t kRayWorkLimit = 4096;
constexpr size_t kSceneBudgetBytes = 512ull * 1024 * 1024;

struct Parameters {
    double inverseAndMin[4];
    double shearAndMax[4];
    uint32_t axesAndCount[4];
    uint32_t sceneAndFlags[4];
    double guard[4];
};
static_assert (sizeof (Parameters) == 128);
static_assert (sizeof (TraversalNode) == 64);
static_assert (sizeof (TraversalTriangle) == 72);

double Milliseconds (Clock::time_point started)
{
    return std::chrono::duration<double, std::milli> (Clock::now () - started).count ();
}

bool RayParameters (const double dir[3], double tmin, double tmax, Parameters& params)
{
    const double length = std::sqrt (dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!(length > 0.0) || !std::isfinite (length) || !std::isfinite (tmin) || !std::isfinite (tmax))
        return false;
    double d[3];
    for (uint32_t axis = 0; axis < 3; ++axis) {
        d[axis] = dir[axis] / length;
        params.inverseAndMin[axis] = d[axis] == 0.0 ? 0.0 : 1.0 / d[axis];
        if (!std::isfinite (params.inverseAndMin[axis]))
            return false;
        if (d[axis] == 0.0)
            params.sceneAndFlags[1] |= 1u << axis;
    }
    uint32_t z = 0;
    for (uint32_t axis = 1; axis < 3; ++axis)
        if (std::abs (d[axis]) > std::abs (d[z]))
            z = axis;
    uint32_t x = (z + 1) % 3;
    uint32_t y = (x + 1) % 3;
    if (d[z] < 0.0)
        std::swap (x, y);
    params.axesAndCount[0] = x;
    params.axesAndCount[1] = y;
    params.axesAndCount[2] = z;
    params.inverseAndMin[3] = std::max (0.0, tmin);
    params.shearAndMax[0] = d[x] / d[z];
    params.shearAndMax[1] = d[y] / d[z];
    params.shearAndMax[2] = 1.0 / d[z];
    params.shearAndMax[3] = tmax > 0.0 ? tmax : std::numeric_limits<double>::max ();
    params.sceneAndFlags[2] = tmax > 0.0 ? 1 : 0;
    return params.shearAndMax[3] > params.inverseAndMin[3];
}
} // namespace

struct SunStudyGpuTraversal::Impl {
    std::shared_ptr<const QueryEngine> engine;
    evp::sunstudy::CpuTraversal cpu;
    mutable std::mutex mutex;
    SunStudyGpuStats stats;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> nodes, triangles, origins, output, staging, constants;
    ComPtr<ID3D11ShaderResourceView> nodeView, triangleView, originView;
    ComPtr<ID3D11UnorderedAccessView> outputView;
    ComPtr<ID3D11Query> completed;
    uint32_t nodeCount = 0;
    double boundsPadding = 0.0;

    explicit Impl (std::shared_ptr<const QueryEngine> source) : engine (std::move (source)), cpu (engine)
    {
    }

    bool Buffer (size_t bytes, uint32_t stride, UINT binds, const void* data, ComPtr<ID3D11Buffer>& buffer,
                 ComPtr<ID3D11ShaderResourceView>* view = nullptr)
    {
        if (bytes == 0 || bytes > std::numeric_limits<UINT>::max ())
            return false;
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = static_cast<UINT> (bytes);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = binds;
        desc.MiscFlags = stride == 0 ? 0 : D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = stride;
        D3D11_SUBRESOURCE_DATA initial {};
        initial.pSysMem = data;
        if (FAILED (device->CreateBuffer (&desc, data == nullptr ? nullptr : &initial, &buffer)))
            return false;
        return view == nullptr || SUCCEEDED (device->CreateShaderResourceView (buffer.Get (), nullptr, &*view));
    }

    void Disable (const std::string& error)
    {
        stats.available = false;
        stats.error = error;
        ArchVizLog ("pipeline: stage=sun-gpu-fallback snapshot=" + std::to_string (cpu.SceneVersion ()) +
                    " reason=" + error);
    }

    bool CreateDevice ()
    {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED (CreateDXGIFactory1 (IID_PPV_ARGS (&factory))))
            return false;
        std::vector<ComPtr<IDXGIAdapter1>> adapters;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1 (i, &adapter) != S_OK)
                break;
            DXGI_ADAPTER_DESC1 desc {};
            if (SUCCEEDED (adapter->GetDesc1 (&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
                adapters.push_back (std::move (adapter));
        }
        // The default D3D adapter may be the integrated display controller on a
        // dual-GPU machine. Prefer discrete memory, but require FP64 capability.
        std::stable_sort (adapters.begin (), adapters.end (), [] (const auto& a, const auto& b) {
            DXGI_ADAPTER_DESC1 left {}, right {};
            a->GetDesc1 (&left);
            b->GetDesc1 (&right);
            return left.DedicatedVideoMemory > right.DedicatedVideoMemory;
        });
        for (const auto& adapter : adapters) {
            const D3D_FEATURE_LEVEL requested[] = { D3D_FEATURE_LEVEL_11_0 };
            device.Reset ();
            context.Reset ();
            if (FAILED (D3D11CreateDevice (adapter.Get (), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, requested, 1,
                                           D3D11_SDK_VERSION, &device, nullptr, &context)))
                continue;
            D3D11_FEATURE_DATA_DOUBLES doubles {};
            if (FAILED (device->CheckFeatureSupport (D3D11_FEATURE_DOUBLES, &doubles, sizeof (doubles))) ||
                !doubles.DoublePrecisionFloatShaderOps)
                continue;
            DXGI_ADAPTER_DESC1 desc {};
            adapter->GetDesc1 (&desc);
            char name[512] {};
            WideCharToMultiByte (CP_UTF8, 0, desc.Description, -1, name, sizeof (name), nullptr, nullptr);
            stats.adapter = name;
            return true;
        }
        context.Reset ();
        device.Reset ();
        return false;
    }

    bool CpuDirectional (const double* positions, size_t count, const double dir[3], double tmin, double tmax,
                         uint8_t* answers, size_t maxParallel, const std::function<bool ()>& isCancelled)
    {
        if (!isCancelled || positions == nullptr || answers == nullptr || dir == nullptr) {
            cpu.OccludeDirectional (positions, count, dir, tmin, tmax, answers, maxParallel);
            stats.cpuFallbackRays += count;
            return !isCancelled || !isCancelled ();
        }
        for (size_t first = 0; first < count; first += kPacketRays) {
            if (isCancelled ())
                return false;
            const size_t size = std::min<size_t> (kPacketRays, count - first);
            cpu.OccludeDirectional (&positions[first * 3], size, dir, tmin, tmax, &answers[first], maxParallel);
            stats.cpuFallbackRays += size;
        }
        return !isCancelled ();
    }

    bool Initialise (const std::function<bool ()>& isCancelled)
    {
        if (stats.attempted)
            return stats.available;
        stats.attempted = true;
        const auto started = Clock::now ();
        // Upper bound: one leaf per triangle plus its branches. Refuse before
        // duplicating a massive CPU scene alongside the viewer's GPU resources.
        if (engine->TriangleCount () > kSceneBudgetBytes / (sizeof (TraversalTriangle) + 2 * sizeof (TraversalNode))) {
            Disable ("traversal scene exceeds conservative 512 MiB GPU budget");
            return false;
        }
        wchar_t enabled[8] {};
        if (GetEnvironmentVariableW (L"TAPIOCA_SUNSTUDY_GPU", enabled, 8) == 1 && enabled[0] == L'0') {
            Disable ("disabled by TAPIOCA_SUNSTUDY_GPU=0");
            return false;
        }
        if (!CreateDevice ()) {
            Disable ("hardware FP64 D3D11 compute unavailable; no precision downgrade");
            return false;
        }
        if (isCancelled && isCancelled ())
            return false;
        ComPtr<ID3DBlob> code, errors;
        if (FAILED (D3DCompile (
                kSunStudyGpuShader, sizeof (kSunStudyGpuShader) - 1, "TapiocaSunStudy", nullptr, nullptr, "main",
                "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_IEEE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                &code, &errors))) {
            Disable (errors == nullptr ? "compute shader compilation failed"
                                       : std::string (static_cast<const char*> (errors->GetBufferPointer ()),
                                                      errors->GetBufferSize ()));
            return false;
        }
        if (FAILED (
                device->CreateComputeShader (code->GetBufferPointer (), code->GetBufferSize (), nullptr, &shader))) {
            Disable ("compute shader creation failed");
            return false;
        }
        const TraversalScene scene = engine->ExportTraversalScene (isCancelled);
        if (isCancelled && isCancelled ())
            return false;
        if (scene.nodes.empty () || scene.nodes.size () > std::numeric_limits<uint32_t>::max ()) {
            Disable ("empty or oversized traversal scene");
            return false;
        }
        nodeCount = static_cast<uint32_t> (scene.nodes.size ());
        double extent = 1.0;
        for (int axis = 0; axis < 3; ++axis)
            extent = std::max ({ extent, std::abs (scene.nodes[0].min[axis]), std::abs (scene.nodes[0].max[axis]) });
        boundsPadding = extent * 64.0 * std::numeric_limits<double>::epsilon ();
        if (!Buffer (scene.nodes.size () * sizeof (TraversalNode), sizeof (TraversalNode), D3D11_BIND_SHADER_RESOURCE,
                     scene.nodes.data (), nodes, &nodeView) ||
            !Buffer (scene.triangles.size () * sizeof (TraversalTriangle), sizeof (TraversalTriangle),
                     D3D11_BIND_SHADER_RESOURCE, scene.triangles.data (), triangles, &triangleView) ||
            !Buffer (kPacketRays * 3 * sizeof (double), 3 * sizeof (double), D3D11_BIND_SHADER_RESOURCE, nullptr,
                     origins, &originView) ||
            !Buffer (kPacketRays * sizeof (uint32_t), sizeof (uint32_t), D3D11_BIND_UNORDERED_ACCESS, nullptr,
                     output) ||
            !Buffer (sizeof (Parameters), 0, D3D11_BIND_CONSTANT_BUFFER, nullptr, constants) ||
            FAILED (device->CreateUnorderedAccessView (output.Get (), nullptr, &outputView))) {
            Disable ("GPU scene or packet allocation failed");
            return false;
        }
        D3D11_BUFFER_DESC readback {};
        readback.ByteWidth = kPacketRays * sizeof (uint32_t);
        readback.Usage = D3D11_USAGE_STAGING;
        readback.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        D3D11_QUERY_DESC query { D3D11_QUERY_EVENT, 0 };
        if (FAILED (device->CreateBuffer (&readback, nullptr, &staging)) ||
            FAILED (device->CreateQuery (&query, &completed))) {
            Disable ("GPU completion/readback allocation failed");
            return false;
        }
        stats.available = true;
        ArchVizLog ("pipeline: stage=sun-gpu-init snapshot=" + std::to_string (cpu.SceneVersion ()) +
                    " backend=d3d11-fp64 adapter=\"" + stats.adapter + "\" nodes=" + std::to_string (nodeCount) +
                    " triangles=" + std::to_string (scene.triangles.size ()) + " sceneBytes=" +
                    std::to_string (scene.nodes.size () * sizeof (TraversalNode) +
                                    scene.triangles.size () * sizeof (TraversalTriangle)) +
                    " packetRays=" + std::to_string (kPacketRays) + " rayWorkLimit=" + std::to_string (kRayWorkLimit) +
                    " wallMs=" + std::to_string (Milliseconds (started)));
        return true;
    }

    bool Packet (const double* positions, uint32_t count, Parameters params, uint32_t* answers,
                 const std::function<bool ()>& isCancelled)
    {
        const auto started = Clock::now ();
        params.axesAndCount[3] = count;
        params.sceneAndFlags[0] = nodeCount;
        params.sceneAndFlags[3] = kRayWorkLimit;
        params.guard[0] = boundsPadding;
        params.guard[1] = 1e-12; // ambiguous arithmetic is resolved by the CPU
        D3D11_BOX box { 0, 0, 0, static_cast<UINT> (count * 3 * sizeof (double)), 1, 1 };
        context->UpdateSubresource (origins.Get (), 0, &box, positions, 0, 0);
        context->UpdateSubresource (constants.Get (), 0, nullptr, &params, 0, 0);
        ID3D11ShaderResourceView* views[] = { nodeView.Get (), triangleView.Get (), originView.Get () };
        ID3D11UnorderedAccessView* uav = outputView.Get ();
        ID3D11Buffer* cb = constants.Get ();
        context->CSSetShader (shader.Get (), nullptr, 0);
        context->CSSetShaderResources (0, 3, views);
        context->CSSetUnorderedAccessViews (0, 1, &uav, nullptr);
        context->CSSetConstantBuffers (0, 1, &cb);
        context->Dispatch ((count + kGroupRays - 1) / kGroupRays, 1, 1);
        uav = nullptr;
        context->CSSetUnorderedAccessViews (0, 1, &uav, nullptr);
        context->CopyResource (staging.Get (), output.Get ());
        context->End (completed.Get ());
        context->Flush ();
        ++stats.dispatches;
        stats.gpuRays += count;
        const auto submitted = Clock::now ();
        stats.submitMilliseconds += std::chrono::duration<double, std::milli> (submitted - started).count ();
        BOOL ready = FALSE;
        for (;;) {
            const HRESULT result =
                context->GetData (completed.Get (), &ready, sizeof (ready), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (result == S_OK && ready)
                break;
            if (isCancelled && isCancelled ()) {
                Disable ("in-flight GPU packet cancelled");
                return false;
            }
            if (FAILED (result) || FAILED (device->GetDeviceRemovedReason ()) || Milliseconds (submitted) > 5000.0) {
                Disable ("GPU packet completion failed or timed out");
                return false;
            }
            // No render/host wait. Yield a low-priority analysis worker while
            // short packets run; back off only for an unusually delayed GPU.
            if (Milliseconds (submitted) > 2.0)
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
            else
                std::this_thread::yield ();
        }
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED (context->Map (staging.Get (), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
            Disable ("completed GPU packet could not be mapped");
            return false;
        }
        std::memcpy (answers, mapped.pData, count * sizeof (uint32_t));
        context->Unmap (staging.Get (), 0);
        stats.readbackMilliseconds += Milliseconds (submitted);
        return true;
    }
};

SunStudyGpuTraversal::SunStudyGpuTraversal (std::shared_ptr<const QueryEngine> engine)
    : impl_ (std::make_unique<Impl> (std::move (engine)))
{
}
SunStudyGpuTraversal::~SunStudyGpuTraversal () = default;

void SunStudyGpuTraversal::OccludeDirectional (const double* origins, size_t count, const double dir[3], double tmin,
                                               double tmax, uint8_t* out, size_t maxParallel) const
{
    OccludeDirectionalCancellable (origins, count, dir, tmin, tmax, out, maxParallel, {});
}

bool SunStudyGpuTraversal::OccludeDirectionalCancellable (const double* origins, size_t count, const double dir[3],
                                                          double tmin, double tmax, uint8_t* out, size_t maxParallel,
                                                          const std::function<bool ()>& isCancelled) const
{
    std::lock_guard<std::mutex> lock (impl_->mutex);
    if (isCancelled && isCancelled ())
        return false;
    Parameters params {};
    if (origins == nullptr || dir == nullptr || out == nullptr || count < kPacketRays || impl_->engine == nullptr ||
        impl_->engine->TriangleCount () == 0 || !RayParameters (dir, tmin, tmax, params) ||
        !impl_->Initialise (isCancelled)) {
        return impl_->CpuDirectional (origins, count, dir, tmin, tmax, out, maxParallel, isCancelled);
    }
    const auto started = Clock::now ();
    const auto previous = impl_->stats;
    uint32_t answers[kPacketRays];
    for (size_t first = 0; first < count; first += kPacketRays) {
        if (isCancelled && isCancelled ())
            return false;
        const uint32_t size = static_cast<uint32_t> (std::min<size_t> (kPacketRays, count - first));
        if (!impl_->Packet (&origins[first * 3], size, params, answers, isCancelled)) {
            if (isCancelled && isCancelled ())
                return false;
            return impl_->CpuDirectional (origins, count, dir, tmin, tmax, out, maxParallel, isCancelled);
        }
        const auto checking = Clock::now ();
        for (uint32_t i = 0; i < size; ++i) {
            if (i % 256 == 0 && isCancelled && isCancelled ())
                return false;
            const double* origin = &origins[(first + i) * 3];
            if (answers[i] > 2 || !std::isfinite (origin[0]) || !std::isfinite (origin[1]) ||
                !std::isfinite (origin[2])) {
                impl_->Disable ("invalid GPU packet input/output");
                return impl_->CpuDirectional (origins, count, dir, tmin, tmax, out, maxParallel, isCancelled);
            }
            if (answers[i] == 2) {
                answers[i] = impl_->engine->Occluded (origin, dir, tmin, tmax) ? 1 : 0;
                ++impl_->stats.cpuFallbackRays;
            }
            // Validate the first packet of EACH direction, then sparse packets.
            // A mismatch disables this backend and replays the WHOLE timestep.
            if (first == 0 || i % 257 == 0) {
                const uint32_t cpu = impl_->engine->Occluded (origin, dir, tmin, tmax) ? 1 : 0;
                ++impl_->stats.validationRays;
                if (answers[i] != cpu) {
                    impl_->Disable ("CPU/GPU parity mismatch");
                    return impl_->CpuDirectional (origins, count, dir, tmin, tmax, out, maxParallel, isCancelled);
                }
            }
            out[first + i] = static_cast<uint8_t> (answers[i]);
        }
        impl_->stats.cpuCheckMilliseconds += Milliseconds (checking);
    }
    ArchVizLog (
        "pipeline: stage=sun-gpu-step snapshot=" + std::to_string (SceneVersion ()) + " backend=d3d11-fp64 rays=" +
        std::to_string (count) + " packets=" + std::to_string (impl_->stats.dispatches - previous.dispatches) +
        " cpuFallbackRays=" + std::to_string (impl_->stats.cpuFallbackRays - previous.cpuFallbackRays) +
        " validationRays=" + std::to_string (impl_->stats.validationRays - previous.validationRays) + " uploadBytes=" +
        std::to_string (count * 3 * sizeof (double) +
                        (impl_->stats.dispatches - previous.dispatches) * sizeof (Parameters)) +
        " readbackBytes=" +
        std::to_string ((impl_->stats.dispatches - previous.dispatches) * kPacketRays * sizeof (uint32_t)) +
        " submitMs=" + std::to_string (impl_->stats.submitMilliseconds - previous.submitMilliseconds) +
        " readbackWaitMs=" + std::to_string (impl_->stats.readbackMilliseconds - previous.readbackMilliseconds) +
        " cpuCheckMs=" + std::to_string (impl_->stats.cpuCheckMilliseconds - previous.cpuCheckMilliseconds) +
        " wallMs=" + std::to_string (Milliseconds (started)));
    return true;
}

void SunStudyGpuTraversal::OccludeRays (const evp::sunstudy::OcclusionRay* rays, size_t count, uint8_t* out,
                                        size_t maxParallel) const
{
    impl_->cpu.OccludeRays (rays, count, out, maxParallel);
}
uint64_t SunStudyGpuTraversal::SceneVersion () const
{
    return impl_->cpu.SceneVersion ();
}
SunStudyGpuStats SunStudyGpuTraversal::Stats () const
{
    std::lock_guard<std::mutex> lock (impl_->mutex);
    return impl_->stats;
}

} // namespace geomsrv::archviz
