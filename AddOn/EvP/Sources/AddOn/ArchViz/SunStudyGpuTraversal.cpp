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
#include <array>
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
constexpr uint32_t kMinGpuRays = 4096;
constexpr uint32_t kPacketRays = 16384;
constexpr size_t kInFlightPackets = 3;
constexpr uint32_t kGroupRays = 64;
constexpr uint32_t kRayWorkLimit = 4096;
constexpr size_t kSceneBudgetBytes = 512ull * 1024 * 1024;

struct Parameters {
    double inverseAndMin[4];
    double shearAndMax[4];
    uint32_t axesAndCount[4];
    uint32_t sceneAndFlags[4];
    double guard[4];
    uint32_t partitions[4];
};
static_assert (sizeof (Parameters) == 144);
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
    struct SceneBuffers {
        ComPtr<ID3D11Buffer> nodes, triangles;
        ComPtr<ID3D11ShaderResourceView> nodeView, triangleView;
        uint32_t nodeCount = 0;
        size_t bytes = 0;
        double padding = 0.0;
    };
    struct PacketBuffers {
        ComPtr<ID3D11Buffer> origins, output, staging, constants;
        ComPtr<ID3D11ShaderResourceView> originView;
        ComPtr<ID3D11UnorderedAccessView> outputView;
        ComPtr<ID3D11Query> completed, timestampBegin, timestampEnd, timestampDisjoint;
        size_t first = 0;
        uint32_t count = 0;
        Clock::time_point submitted;
    };
    std::shared_ptr<const QueryEngine> engine, contextEngine;
    evp::sunstudy::SunStudyPartitionTraversal cpu;
    mutable std::mutex mutex;
    std::shared_ptr<std::mutex> deviceMutex = std::make_shared<std::mutex> ();
    SunStudyGpuStats stats;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;
    std::array<PacketBuffers, kInFlightPackets> packets;
    HANDLE yieldTimer = nullptr;
    std::shared_ptr<SceneBuffers> analysisScene, contextScene;

    Impl (std::shared_ptr<const QueryEngine> source, std::shared_ptr<const QueryEngine> contextSource,
          const Impl* previous)
        : engine (std::move (source)), contextEngine (std::move (contextSource)), cpu (engine, contextEngine)
    {
        if (previous == nullptr)
            return;
        std::lock_guard<std::mutex> lock (previous->mutex);
        // A fully sample-reused generation never initialises its inherited
        // backend. Carry its proven immutable buffers through that generation.
        if (!previous->stats.available &&
            (previous->stats.attempted || previous->device == nullptr || previous->shader == nullptr))
            return;
        device = previous->device;
        context = previous->context;
        shader = previous->shader;
        deviceMutex = previous->deviceMutex;
        stats.deviceReused = true;
        stats.adapter = previous->stats.adapter;
        if (engine == previous->engine)
            analysisScene = previous->analysisScene;
        if (contextEngine != nullptr && contextEngine == previous->contextEngine) {
            contextScene = previous->contextScene;
            stats.contextReused = contextScene != nullptr;
        }
    }

    ~Impl ()
    {
        if (yieldTimer != nullptr)
            CloseHandle (yieldTimer);
    }

    void YieldPacket ()
    {
        const auto started = Clock::now ();
        LARGE_INTEGER due;
        due.QuadPart = -10000; // 1 ms, relative, in 100 ns units
        if (yieldTimer != nullptr && SetWaitableTimer (yieldTimer, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject (yieldTimer, INFINITE);
        else
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        stats.pollSleepMilliseconds += Milliseconds (started);
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

    bool BuildScene (const std::shared_ptr<const QueryEngine>& source, std::shared_ptr<SceneBuffers>& target,
                     const std::function<bool ()>& isCancelled)
    {
        if (target != nullptr)
            return true;
        target = std::make_shared<SceneBuffers> ();
        if (source == nullptr || source->TriangleCount () == 0)
            return true;
        const TraversalScene scene = source->ExportTraversalScene (isCancelled);
        if ((isCancelled && isCancelled ()) || scene.nodes.empty () ||
            scene.nodes.size () > std::numeric_limits<uint32_t>::max ())
            return false;
        target->nodeCount = static_cast<uint32_t> (scene.nodes.size ());
        double extent = 1.0;
        for (int axis = 0; axis < 3; ++axis)
            extent = std::max ({ extent, std::abs (scene.nodes[0].min[axis]), std::abs (scene.nodes[0].max[axis]) });
        target->padding = extent * 64.0 * std::numeric_limits<double>::epsilon ();
        target->bytes =
            scene.nodes.size () * sizeof (TraversalNode) + scene.triangles.size () * sizeof (TraversalTriangle);
        if (!Buffer (scene.nodes.size () * sizeof (TraversalNode), sizeof (TraversalNode), D3D11_BIND_SHADER_RESOURCE,
                     scene.nodes.data (), target->nodes, &target->nodeView) ||
            !Buffer (scene.triangles.size () * sizeof (TraversalTriangle), sizeof (TraversalTriangle),
                     D3D11_BIND_SHADER_RESOURCE, scene.triangles.data (), target->triangles, &target->triangleView))
            return false;
        stats.sceneUploadedBytes += target->bytes;
        return true;
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
        for (size_t first = 0; first < count; first += kMinGpuRays) {
            if (isCancelled ())
                return false;
            const size_t size = std::min<size_t> (kMinGpuRays, count - first);
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
        const size_t triangleCount =
            engine->TriangleCount () + (contextEngine != nullptr ? contextEngine->TriangleCount () : 0);
        if (triangleCount > kSceneBudgetBytes / (sizeof (TraversalTriangle) + 2 * sizeof (TraversalNode))) {
            Disable ("traversal scene exceeds conservative 512 MiB GPU budget");
            return false;
        }
        wchar_t enabled[8] {};
        if (GetEnvironmentVariableW (L"TAPIOCA_SUNSTUDY_GPU", enabled, 8) == 1 && enabled[0] == L'0') {
            Disable ("disabled by TAPIOCA_SUNSTUDY_GPU=0");
            return false;
        }
        if (device == nullptr && !CreateDevice ()) {
            Disable ("hardware FP64 D3D11 compute unavailable; no precision downgrade");
            return false;
        }
        if (isCancelled && isCancelled ())
            return false;
        if (shader == nullptr) {
            ComPtr<ID3DBlob> code, errors;
            if (FAILED (D3DCompile (kSunStudyGpuShader, sizeof (kSunStudyGpuShader) - 1, "TapiocaSunStudy", nullptr,
                                    nullptr, "main", "cs_5_0",
                                    D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_IEEE_STRICTNESS |
                                        D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                    0, &code, &errors))) {
                Disable (errors == nullptr ? "compute shader compilation failed"
                                           : std::string (static_cast<const char*> (errors->GetBufferPointer ()),
                                                          errors->GetBufferSize ()));
                return false;
            }
            if (FAILED (device->CreateComputeShader (code->GetBufferPointer (), code->GetBufferSize (), nullptr,
                                                     &shader))) {
                Disable ("compute shader creation failed");
                return false;
            }
        }
        const size_t beforeContext = stats.sceneUploadedBytes;
        if (!BuildScene (contextEngine, contextScene, isCancelled) ||
            !BuildScene (engine, analysisScene, isCancelled)) {
            Disable ("GPU traversal scene allocation failed or cancelled");
            return false;
        }
        stats.contextUploadedBytes = stats.contextReused ? 0 : contextScene->bytes;
        if (isCancelled && isCancelled ())
            return false;
        D3D11_BUFFER_DESC readback {};
        readback.ByteWidth = kPacketRays * sizeof (uint32_t);
        readback.Usage = D3D11_USAGE_STAGING;
        readback.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        D3D11_QUERY_DESC query { D3D11_QUERY_EVENT, 0 };
        // Optional diagnostics. The existing result event covers these queries;
        // their values are read once, never waited on or polled separately.
        D3D11_QUERY_DESC timestamp { D3D11_QUERY_TIMESTAMP, 0 };
        D3D11_QUERY_DESC disjoint { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        for (auto& packet : packets) {
            if (!Buffer (kPacketRays * 3 * sizeof (double), 3 * sizeof (double), D3D11_BIND_SHADER_RESOURCE, nullptr,
                         packet.origins, &packet.originView) ||
                !Buffer (kPacketRays * sizeof (uint32_t), sizeof (uint32_t), D3D11_BIND_UNORDERED_ACCESS, nullptr,
                         packet.output) ||
                !Buffer (sizeof (Parameters), 0, D3D11_BIND_CONSTANT_BUFFER, nullptr, packet.constants) ||
                FAILED (device->CreateUnorderedAccessView (packet.output.Get (), nullptr, &packet.outputView)) ||
                FAILED (device->CreateBuffer (&readback, nullptr, &packet.staging)) ||
                FAILED (device->CreateQuery (&query, &packet.completed))) {
                Disable ("GPU packet ring allocation failed");
                return false;
            }
            if (FAILED (device->CreateQuery (&timestamp, &packet.timestampBegin)) ||
                FAILED (device->CreateQuery (&timestamp, &packet.timestampEnd)) ||
                FAILED (device->CreateQuery (&disjoint, &packet.timestampDisjoint))) {
                packet.timestampBegin.Reset ();
                packet.timestampEnd.Reset ();
                packet.timestampDisjoint.Reset ();
            }
        }
        // Sleep(1) can oversleep by a full Windows timer tick per packet. Use a
        // worker-local high-resolution timer without changing host timer policy.
        yieldTimer = CreateWaitableTimerExW (nullptr, nullptr, 0x00000002, TIMER_MODIFY_STATE | SYNCHRONIZE);
        stats.available = true;
        ArchVizLog ("pipeline: stage=sun-gpu-init snapshot=" + std::to_string (cpu.SceneVersion ()) +
                    " backend=d3d11-fp64 adapter=\"" + stats.adapter +
                    "\" nodes=" + std::to_string (analysisScene->nodeCount + contextScene->nodeCount) +
                    " triangles=" + std::to_string (triangleCount) +
                    " sceneBytes=" + std::to_string (analysisScene->bytes + contextScene->bytes) +
                    " sceneUploadedBytes=" + std::to_string (stats.sceneUploadedBytes - beforeContext) +
                    " contextUploadedBytes=" + std::to_string (stats.contextUploadedBytes) + " contextReused=" +
                    std::to_string (stats.contextReused) + " deviceReused=" + std::to_string (stats.deviceReused) +
                    " packetRays=" + std::to_string (kPacketRays) + " rayWorkLimit=" + std::to_string (kRayWorkLimit) +
                    " wallMs=" + std::to_string (Milliseconds (started)));
        return true;
    }

    void SubmitPacket (PacketBuffers& packet, const double* positions, size_t first, uint32_t count, Parameters params)
    {
        const auto started = Clock::now ();
        params.axesAndCount[3] = count;
        params.sceneAndFlags[0] = analysisScene->nodeCount;
        params.partitions[0] = contextScene->nodeCount;
        params.sceneAndFlags[3] = kRayWorkLimit;
        params.guard[0] = std::max (analysisScene->padding, contextScene->padding);
        params.guard[1] = 1e-12; // ambiguous arithmetic is resolved by the CPU
        D3D11_BOX box { 0, 0, 0, static_cast<UINT> (count * 3 * sizeof (double)), 1, 1 };
        context->UpdateSubresource (packet.origins.Get (), 0, &box, positions, 0, 0);
        context->UpdateSubresource (packet.constants.Get (), 0, nullptr, &params, 0, 0);
        ID3D11ShaderResourceView* views[] = { analysisScene->nodeView.Get (), analysisScene->triangleView.Get (),
                                              packet.originView.Get (), contextScene->nodeView.Get (),
                                              contextScene->triangleView.Get () };
        ID3D11UnorderedAccessView* uav = packet.outputView.Get ();
        ID3D11Buffer* cb = packet.constants.Get ();
        context->CSSetShader (shader.Get (), nullptr, 0);
        context->CSSetShaderResources (0, 5, views);
        context->CSSetUnorderedAccessViews (0, 1, &uav, nullptr);
        context->CSSetConstantBuffers (0, 1, &cb);
        if (packet.timestampDisjoint != nullptr) {
            context->Begin (packet.timestampDisjoint.Get ());
            context->End (packet.timestampBegin.Get ());
        }
        context->Dispatch ((count + kGroupRays - 1) / kGroupRays, 1, 1);
        if (packet.timestampDisjoint != nullptr) {
            context->End (packet.timestampEnd.Get ());
            context->End (packet.timestampDisjoint.Get ());
        }
        uav = nullptr;
        context->CSSetUnorderedAccessViews (0, 1, &uav, nullptr);
        D3D11_BOX resultBox { 0, 0, 0, static_cast<UINT> (count * sizeof (uint32_t)), 1, 1 };
        context->CopySubresourceRegion (packet.staging.Get (), 0, 0, 0, 0, packet.output.Get (), 0, &resultBox);
        context->End (packet.completed.Get ());
        ++stats.dispatches;
        stats.gpuRays += count;
        packet.first = first;
        packet.count = count;
        packet.submitted = Clock::now ();
        stats.submitMilliseconds += Milliseconds (started);
    }

    bool ReadPacket (PacketBuffers& packet, uint32_t* answers, const std::function<bool ()>& isCancelled)
    {
        const auto waiting = Clock::now ();
        BOOL ready = FALSE;
        for (;;) {
            const HRESULT result =
                context->GetData (packet.completed.Get (), &ready, sizeof (ready), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (result == S_OK && ready)
                break;
            if (isCancelled && isCancelled ()) {
                Disable ("in-flight GPU packet cancelled");
                return false;
            }
            if (FAILED (result) || FAILED (device->GetDeviceRemovedReason ()) ||
                Milliseconds (packet.submitted) > 5000.0) {
                Disable ("GPU packet completion failed or timed out");
                return false;
            }
            // No render/host wait. Yield a low-priority analysis worker while
            // short packets run; back off only for an unusually delayed GPU.
            if (Milliseconds (waiting) > 2.0)
                YieldPacket ();
            else
                std::this_thread::yield ();
        }
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED (context->Map (packet.staging.Get (), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
            Disable ("completed GPU packet could not be mapped");
            return false;
        }
        std::memcpy (answers, mapped.pData, packet.count * sizeof (uint32_t));
        context->Unmap (packet.staging.Get (), 0);
        stats.readbackBytes += packet.count * sizeof (uint32_t);
        stats.readbackMilliseconds += Milliseconds (waiting);
        if (packet.timestampDisjoint != nullptr) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing {};
            UINT64 begin = 0, end = 0;
            constexpr UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
            if (context->GetData (packet.timestampDisjoint.Get (), &timing, sizeof (timing), flags) == S_OK &&
                !timing.Disjoint && timing.Frequency > 0 &&
                context->GetData (packet.timestampBegin.Get (), &begin, sizeof (begin), flags) == S_OK &&
                context->GetData (packet.timestampEnd.Get (), &end, sizeof (end), flags) == S_OK && end >= begin) {
                stats.computeMilliseconds +=
                    static_cast<double> (end - begin) * 1000.0 / static_cast<double> (timing.Frequency);
                ++stats.timedDispatches;
            }
        }
        return true;
    }

    bool CheckPacket (const double* positions, size_t first, uint32_t count, const double dir[3], double tmin,
                      double tmax, const uint32_t* answers, uint8_t* out, size_t maxParallel,
                      const std::function<bool ()>& isCancelled)
    {
        const auto checking = Clock::now ();
        std::vector<double> checkPositions;
        std::vector<uint32_t> indices;
        for (uint32_t i = 0; i < count; ++i) {
            if (i % 256 == 0 && isCancelled && isCancelled ())
                return false;
            const double* origin = &positions[(first + i) * 3];
            if (answers[i] > 3 || !std::isfinite (origin[0]) || !std::isfinite (origin[1]) ||
                !std::isfinite (origin[2])) {
                Disable ("invalid GPU packet input/output");
                return false;
            }
            // Keep full validation of the first 4096 rays of EVERY direction,
            // then sparse checks. Ambiguous rays remain exact, not sampled.
            const bool validate = first + i < kMinGpuRays || (first + i) % 257 == 0;
            stats.validationRays += validate ? 1 : 0;
            if (answers[i] >= 2) {
                ++stats.cpuFallbackRays;
                stats.ambiguousRays += answers[i] == 2 ? 1 : 0;
                stats.workLimitRays += answers[i] == 3 ? 1 : 0;
            }
            if (answers[i] >= 2 || validate) {
                indices.push_back (i);
                checkPositions.insert (checkPositions.end (), origin, origin + 3);
            }
            else
                out[first + i] = static_cast<uint8_t> (answers[i]);
        }
        std::vector<uint8_t> checks (indices.size ());
        // Chunking bounds cancellation latency; the CPU baseline's parallel
        // packet tracer replaces thousands of serial single-ray calls.
        for (size_t offset = 0; offset < indices.size (); offset += kMinGpuRays) {
            if (isCancelled && isCancelled ())
                return false;
            const size_t size = std::min<size_t> (kMinGpuRays, indices.size () - offset);
            cpu.OccludeDirectional (&checkPositions[offset * 3], size, dir, tmin, tmax, &checks[offset], maxParallel);
            stats.cpuCheckRays += size;
        }
        for (size_t j = 0; j < indices.size (); ++j) {
            const uint32_t i = indices[j];
            if (answers[i] < 2 && answers[i] != checks[j]) {
                Disable ("CPU/GPU parity mismatch");
                return false;
            }
            out[first + i] = checks[j];
        }
        stats.cpuCheckMilliseconds += Milliseconds (checking);
        return !isCancelled || !isCancelled ();
    }
};

SunStudyGpuTraversal::SunStudyGpuTraversal (std::shared_ptr<const QueryEngine> engine,
                                            std::shared_ptr<const QueryEngine> context,
                                            const SunStudyGpuTraversal* previous)
    : impl_ (std::make_unique<Impl> (std::move (engine), std::move (context),
                                     previous != nullptr ? previous->impl_.get () : nullptr))
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
    std::lock_guard<std::mutex> deviceLock (*impl_->deviceMutex);
    if (isCancelled && isCancelled ())
        return false;
    Parameters params {};
    if (origins == nullptr || dir == nullptr || out == nullptr || count < kMinGpuRays || impl_->engine == nullptr ||
        (impl_->engine->TriangleCount () == 0 &&
         (impl_->contextEngine == nullptr || impl_->contextEngine->TriangleCount () == 0)) ||
        !RayParameters (dir, tmin, tmax, params) || !impl_->Initialise (isCancelled)) {
        return impl_->CpuDirectional (origins, count, dir, tmin, tmax, out, maxParallel, isCancelled);
    }
    const auto started = Clock::now ();
    const auto previous = impl_->stats;
    std::vector<uint32_t> answers (kPacketRays);
    size_t next = 0;
    const auto submit = [&] (Impl::PacketBuffers& packet) {
        const uint32_t size = static_cast<uint32_t> (std::min<size_t> (kPacketRays, count - next));
        impl_->SubmitPacket (packet, &origins[next * 3], next, size, params);
        next += size;
    };
    const size_t inFlight = std::min<size_t> (kInFlightPackets, (count + kPacketRays - 1) / kPacketRays);
    impl_->stats.maxInFlight = std::max (impl_->stats.maxInFlight, inFlight);
    for (size_t p = 0; p < inFlight; ++p)
        submit (impl_->packets[p]);
    impl_->context->Flush ();
    for (size_t p = 0, resolved = 0; resolved < count; ++p) {
        if (isCancelled && isCancelled ())
            return false;
        auto& packet = impl_->packets[p % inFlight];
        const size_t first = packet.first;
        const uint32_t size = packet.count;
        if (!impl_->ReadPacket (packet, answers.data (), isCancelled)) {
            if (isCancelled && isCancelled ())
                return false;
            return impl_->CpuDirectional (origins, count, dir, tmin, tmax, out, maxParallel, isCancelled);
        }
        // Refill before CPU checks so the GPU and exact CPU work overlap. A
        // failure still replays the whole timestep, never a mixed partial day.
        if (next < count) {
            submit (packet);
            impl_->context->Flush ();
        }
        if (!impl_->CheckPacket (origins, first, size, dir, tmin, tmax, answers.data (), out, maxParallel,
                                 isCancelled)) {
            if (isCancelled && isCancelled ())
                return false;
            return impl_->CpuDirectional (origins, count, dir, tmin, tmax, out, maxParallel, isCancelled);
        }
        resolved += size;
    }
    ArchVizLog (
        "pipeline: stage=sun-gpu-step snapshot=" + std::to_string (SceneVersion ()) + " backend=d3d11-fp64 rays=" +
        std::to_string (count) + " packets=" + std::to_string (impl_->stats.dispatches - previous.dispatches) +
        " cpuFallbackRays=" + std::to_string (impl_->stats.cpuFallbackRays - previous.cpuFallbackRays) +
        " ambiguousRays=" + std::to_string (impl_->stats.ambiguousRays - previous.ambiguousRays) +
        " workLimitRays=" + std::to_string (impl_->stats.workLimitRays - previous.workLimitRays) +
        " cpuCheckRays=" + std::to_string (impl_->stats.cpuCheckRays - previous.cpuCheckRays) +
        " maxInFlight=" + std::to_string (inFlight) +
        " validationRays=" + std::to_string (impl_->stats.validationRays - previous.validationRays) + " uploadBytes=" +
        std::to_string (count * 3 * sizeof (double) +
                        (impl_->stats.dispatches - previous.dispatches) * sizeof (Parameters)) +
        " readbackBytes=" + std::to_string (impl_->stats.readbackBytes - previous.readbackBytes) +
        " submitMs=" + std::to_string (impl_->stats.submitMilliseconds - previous.submitMilliseconds) +
        " readbackWaitMs=" + std::to_string (impl_->stats.readbackMilliseconds - previous.readbackMilliseconds) +
        " cpuCheckMs=" + std::to_string (impl_->stats.cpuCheckMilliseconds - previous.cpuCheckMilliseconds) +
        " pollSleepMs=" + std::to_string (impl_->stats.pollSleepMilliseconds - previous.pollSleepMilliseconds) +
        " gpuComputeMs=" + std::to_string (impl_->stats.computeMilliseconds - previous.computeMilliseconds) +
        " timedPackets=" + std::to_string (impl_->stats.timedDispatches - previous.timedDispatches) +
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
