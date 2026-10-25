#include "DxrWorker/DxrBackend.hpp"

#include "DxrWorker/DxrDevice.hpp"
#include "DxrWorker/DxrOcclusionDxil.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace evp::dxrworker {

namespace {

using rayworker::GpuReason;
namespace packet = rayworker::packet;
namespace protocol = rayworker::protocol;

uint32_t Microseconds (std::chrono::steady_clock::time_point since)
{
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds> (std::chrono::steady_clock::now () - since);
    return static_cast<uint32_t> (std::min<long long> (elapsed.count (), UINT32_MAX));
}

D3D12_RESOURCE_BARRIER UavBarrier (ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
}

// Root parameter slots; DxrOcclusion.hlsl's registers.
enum RootSlot : UINT { SlotScene = 0, SlotPositions, SlotRays, SlotDirections, SlotOccluded, SlotLaunch, SlotCount };

constexpr uint64_t kRecordStride = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;
constexpr uint64_t kTableAlign = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;

class DxrBackend final : public rayworker::RayBackend {
  public:
    explicit DxrBackend (const DxrBackendOptions& options) : options_ (options)
    {
        device_ = DxrDevice::Create (capability_);
        if (device_ != nullptr && !CreatePipeline ()) {
            device_.reset ();
            capability_.reason = GpuReason::WorkerFailed;
        }
    }

    protocol::HelloPayload Describe () const override
    {
        protocol::HelloPayload hello;
        hello.reason = capability_.reason;
        if (capability_.reason == GpuReason::None)
            hello.capabilities = protocol::CapabilityHardwareRayTracing;
        hello.driverVersion = capability_.driverVersion;
        hello.runtimeVersion = capability_.raytracingTier; // 10 = tier 1.0, 11 = tier 1.1
        hello.deviceMemoryBytes = capability_.dedicatedMemoryBytes;
        hello.deviceName = capability_.reason == GpuReason::None ? capability_.deviceName : capability_.message;
        return hello;
    }

    GpuReason BuildScene (const packet::Scene& scene, protocol::SceneReadyPayload& ready, std::string& error) override
    {
        const auto started = std::chrono::steady_clock::now ();
        tlas_.Reset ();
        blas_.Reset ();
        sceneTriangles_ = 0;
        if (device_ == nullptr) {
            error = "no DXR device: " + capability_.message;
            return capability_.reason;
        }
        const size_t vertexCount = scene.vertices.size () / 3;
        const size_t triangleCount = scene.triangles.size () / 3;
        origin_[0] = origin_[1] = origin_[2] = 0.0;
        if (triangleCount == 0) {
            ready.buildMicroseconds = Microseconds (started);
            return GpuReason::None; // nothing occludes: every traced ray is clear
        }

        // ⚠️ ONE SHARED ORIGIN, SUBTRACTED IN DOUBLE, BEFORE ANY f32. A survey
        // coordinate of 10^6 m keeps ~6 cm of f32 resolution; centred, the same
        // building keeps sub-millimetre. Rays are moved by the same origin.
        double lo[3] = { std::numeric_limits<double>::max (), std::numeric_limits<double>::max (),
                         std::numeric_limits<double>::max () };
        double hi[3] = { -lo[0], -lo[1], -lo[2] };
        for (size_t v = 0; v < vertexCount; ++v)
            for (int a = 0; a < 3; ++a) {
                const double x = scene.vertices[v * 3 + a];
                if (!std::isfinite (x)) {
                    error = "the scene has a non-finite vertex";
                    return GpuReason::ValidationFailed;
                }
                lo[a] = std::min (lo[a], x);
                hi[a] = std::max (hi[a], x);
            }
        for (int a = 0; a < 3; ++a)
            origin_[a] = 0.5 * (lo[a] + hi[a]);
        std::vector<float> local (vertexCount * 3);
        for (size_t v = 0; v < vertexCount; ++v)
            for (int a = 0; a < 3; ++a)
                local[v * 3 + a] = static_cast<float> (scene.vertices[v * 3 + a] - origin_[a]);

        HRESULT hr = device_->Begin ();
        ComPtr<ID3D12Resource> vertices, indices;
        if (SUCCEEDED (hr))
            hr = device_->Upload (local.data (), local.size () * sizeof (float),
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, vertices);
        if (SUCCEEDED (hr))
            hr = device_->Upload (scene.triangles.data (), scene.triangles.size () * sizeof (uint32_t),
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, indices);
        if (FAILED (hr))
            return device_->ReasonOf (hr, "scene upload", error);

        D3D12_RAYTRACING_GEOMETRY_DESC geometry {};
        geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geometry.Triangles.VertexBuffer.StartAddress = vertices->GetGPUVirtualAddress ();
        geometry.Triangles.VertexBuffer.StrideInBytes = 3 * sizeof (float);
        geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometry.Triangles.VertexCount = static_cast<UINT> (vertexCount);
        geometry.Triangles.IndexBuffer = indices->GetGPUVirtualAddress ();
        geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        geometry.Triangles.IndexCount = static_cast<UINT> (scene.triangles.size ());

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom {};
        bottom.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bottom.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        bottom.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        bottom.NumDescs = 1;
        bottom.pGeometryDescs = &geometry;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottomInfo {};
        device_->Device ()->GetRaytracingAccelerationStructurePrebuildInfo (&bottom, &bottomInfo);

        D3D12_RAYTRACING_INSTANCE_DESC instance {};
        instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1.0f;
        instance.InstanceMask = 0xFF;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top {};
        top.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        top.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        top.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        top.NumDescs = 1;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topInfo {};
        device_->Device ()->GetRaytracingAccelerationStructurePrebuildInfo (&top, &topInfo);

        const auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const auto asState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
        ComPtr<ID3D12Resource> scratch, blas, tlas, instances;
        hr = device_->CreateBuffer (std::max (bottomInfo.ScratchDataSizeInBytes, topInfo.ScratchDataSizeInBytes),
                                    D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, uav, scratch);
        if (SUCCEEDED (hr))
            hr = device_->CreateBuffer (bottomInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, asState, uav,
                                        blas);
        if (SUCCEEDED (hr))
            hr = device_->CreateBuffer (topInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, asState, uav, tlas);
        if (SUCCEEDED (hr)) {
            instance.AccelerationStructure = blas->GetGPUVirtualAddress ();
            hr = device_->CreateBuffer (sizeof (instance), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                                        D3D12_RESOURCE_FLAG_NONE, instances);
        }
        if (SUCCEEDED (hr)) {
            void* mapped = nullptr;
            hr = instances->Map (0, nullptr, &mapped);
            if (SUCCEEDED (hr)) {
                std::memcpy (mapped, &instance, sizeof (instance));
                instances->Unmap (0, nullptr);
            }
        }
        if (FAILED (hr))
            return device_->ReasonOf (hr, "acceleration structure allocation", error);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build {};
        build.Inputs = bottom;
        build.DestAccelerationStructureData = blas->GetGPUVirtualAddress ();
        build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress ();
        device_->List ()->BuildRaytracingAccelerationStructure (&build, 0, nullptr);
        // The top level reads the bottom level and reuses its scratch.
        const D3D12_RESOURCE_BARRIER afterBottom[] = { UavBarrier (blas.Get ()), UavBarrier (scratch.Get ()) };
        device_->List ()->ResourceBarrier (2, afterBottom);
        top.InstanceDescs = instances->GetGPUVirtualAddress ();
        build.Inputs = top;
        build.DestAccelerationStructureData = tlas->GetGPUVirtualAddress ();
        device_->List ()->BuildRaytracingAccelerationStructure (&build, 0, nullptr);
        const D3D12_RESOURCE_BARRIER afterTop = UavBarrier (tlas.Get ());
        device_->List ()->ResourceBarrier (1, &afterTop);
        device_->Keep (vertices);
        device_->Keep (indices);
        device_->Keep (scratch);
        device_->Keep (instances);
        hr = device_->Submit (options_.submitTimeoutMs);
        if (FAILED (hr))
            return device_->ReasonOf (hr, "acceleration structure build", error);

        blas_ = blas;
        tlas_ = tlas;
        sceneTriangles_ = triangleCount;
        ready.buildMicroseconds = Microseconds (started);
        ready.deviceBytes = bottomInfo.ResultDataMaxSizeInBytes + topInfo.ResultDataMaxSizeInBytes;
        return GpuReason::None;
    }

    GpuReason Trace (const packet::Request& request, packet::Result& result, protocol::RunTimings& timings,
                     const std::function<bool ()>& isCancelled, std::string& error) override;

  private:
    bool CreatePipeline ();
    GpuReason Dispatch (const std::vector<float>& positions, const std::vector<uint32_t>& rays,
                        const std::vector<float>& directions, float tmin, float tmax, std::vector<uint32_t>& occluded,
                        const std::function<bool ()>& isCancelled, std::string& error);

    DxrBackendOptions options_;
    DxrCapability capability_;
    std::unique_ptr<DxrDevice> device_;
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12StateObject> pipeline_;
    ComPtr<ID3D12Resource> shaderTable_;
    ComPtr<ID3D12Resource> blas_;
    ComPtr<ID3D12Resource> tlas_;
    size_t sceneTriangles_ = 0;
    double origin_[3] = { 0.0, 0.0, 0.0 };
};

bool DxrBackend::CreatePipeline ()
{
    D3D12_ROOT_PARAMETER params[SlotCount] {};
    const D3D12_ROOT_PARAMETER_TYPE kinds[SlotCount] = {
        D3D12_ROOT_PARAMETER_TYPE_SRV, D3D12_ROOT_PARAMETER_TYPE_SRV, D3D12_ROOT_PARAMETER_TYPE_SRV,
        D3D12_ROOT_PARAMETER_TYPE_SRV, D3D12_ROOT_PARAMETER_TYPE_UAV, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
    };
    const UINT registers[SlotCount] = { 0, 1, 2, 3, 0, 0 };
    for (UINT i = 0; i < SlotCount; ++i) {
        params[i].ParameterType = kinds[i];
        params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        if (kinds[i] == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
            params[i].Constants.ShaderRegister = registers[i];
            params[i].Constants.Num32BitValues = 4;
        }
        else
            params[i].Descriptor.ShaderRegister = registers[i];
    }
    D3D12_ROOT_SIGNATURE_DESC rootDesc {};
    rootDesc.NumParameters = SlotCount;
    rootDesc.pParameters = params;
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED (D3D12SerializeRootSignature (&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED (device_->Device ()->CreateRootSignature (0, blob->GetBufferPointer (), blob->GetBufferSize (),
                                                         IID_PPV_ARGS (&rootSignature_)))) {
        capability_.message = "root signature creation failed";
        return false;
    }

    D3D12_DXIL_LIBRARY_DESC library {};
    library.DXILLibrary.pShaderBytecode = kDxrOcclusionDxil;
    library.DXILLibrary.BytecodeLength = sizeof (kDxrOcclusionDxil);
    D3D12_HIT_GROUP_DESC hitGroup {};
    hitGroup.HitGroupExport = L"HitGroup";
    hitGroup.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hitGroup.ClosestHitShaderImport = L"ClosestHit";
    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig {};
    shaderConfig.MaxPayloadSizeInBytes = sizeof (uint32_t);
    shaderConfig.MaxAttributeSizeInBytes = 2 * sizeof (float);
    D3D12_GLOBAL_ROOT_SIGNATURE globalRoot { rootSignature_.Get () };
    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig {};
    pipelineConfig.MaxTraceRecursionDepth = 1;
    const D3D12_STATE_SUBOBJECT subobjects[] = {
        { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library },
        { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitGroup },
        { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig },
        { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRoot },
        { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig },
    };
    D3D12_STATE_OBJECT_DESC stateDesc {};
    stateDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    stateDesc.NumSubobjects = static_cast<UINT> (std::size (subobjects));
    stateDesc.pSubobjects = subobjects;
    if (FAILED (device_->Device ()->CreateStateObject (&stateDesc, IID_PPV_ARGS (&pipeline_)))) {
        capability_.message = "DXR pipeline creation failed (precompiled DXIL rejected?)";
        return false;
    }

    // Shader table: ray generation, miss and hit group, each its own table.
    ComPtr<ID3D12StateObjectProperties> properties;
    if (FAILED (pipeline_.As (&properties)) ||
        FAILED (device_->CreateBuffer (3 * kTableAlign, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                                       D3D12_RESOURCE_FLAG_NONE, shaderTable_))) {
        capability_.message = "shader table creation failed";
        return false;
    }
    uint8_t* mapped = nullptr;
    if (FAILED (shaderTable_->Map (0, nullptr, reinterpret_cast<void**> (&mapped)))) {
        capability_.message = "shader table map failed";
        return false;
    }
    const wchar_t* exports[] = { L"RayGen", L"Miss", L"HitGroup" };
    for (int i = 0; i < 3; ++i) {
        const void* id = properties->GetShaderIdentifier (exports[i]);
        if (id == nullptr) {
            shaderTable_->Unmap (0, nullptr);
            capability_.message = "a shader identifier is missing from the DXR pipeline";
            return false;
        }
        std::memcpy (mapped + i * kTableAlign, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    }
    shaderTable_->Unmap (0, nullptr);
    return true;
}

GpuReason DxrBackend::Dispatch (const std::vector<float>& positions, const std::vector<uint32_t>& rays,
                                const std::vector<float>& directions, float tmin, float tmax,
                                std::vector<uint32_t>& occluded, const std::function<bool ()>& isCancelled,
                                std::string& error)
{
    const size_t rayCount = rays.size () / 2;
    occluded.assign (rayCount, 0u);
    if (rayCount == 0)
        return GpuReason::None;
    const uint64_t outBytes = rayCount * sizeof (uint32_t);

    HRESULT hr = device_->Begin ();
    ComPtr<ID3D12Resource> positionBuffer, rayBuffer, directionBuffer, outBuffer, readback;
    const auto srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (SUCCEEDED (hr))
        hr = device_->Upload (positions.data (), positions.size () * sizeof (float), srv, positionBuffer);
    if (SUCCEEDED (hr))
        hr = device_->Upload (rays.data (), rays.size () * sizeof (uint32_t), srv, rayBuffer);
    if (SUCCEEDED (hr))
        hr = device_->Upload (directions.data (), directions.size () * sizeof (float), srv, directionBuffer);
    if (SUCCEEDED (hr))
        hr = device_->CreateBuffer (outBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, outBuffer);
    if (SUCCEEDED (hr))
        hr = device_->CreateBuffer (outBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                                    D3D12_RESOURCE_FLAG_NONE, readback);
    if (SUCCEEDED (hr))
        hr = device_->Submit (options_.submitTimeoutMs);
    if (FAILED (hr))
        return device_->ReasonOf (hr, "ray upload", error);

    const D3D12_GPU_VIRTUAL_ADDRESS table = shaderTable_->GetGPUVirtualAddress ();
    for (size_t base = 0; base < rayCount; base += options_.raysPerLaunch) {
        // ⚠️ CANCELLATION BETWEEN LAUNCHES ONLY. A launch in flight completes;
        // its words are simply never read, so no partial step is committed.
        if (isCancelled && isCancelled ()) {
            error = "the run was cancelled";
            return GpuReason::WorkerFailed;
        }
        const UINT count = static_cast<UINT> (std::min<size_t> (options_.raysPerLaunch, rayCount - base));
        hr = device_->Begin ();
        if (FAILED (hr))
            return device_->ReasonOf (hr, "launch", error);
        ID3D12GraphicsCommandList4* list = device_->List ();
        list->SetComputeRootSignature (rootSignature_.Get ());
        list->SetComputeRootShaderResourceView (SlotScene, tlas_->GetGPUVirtualAddress ());
        list->SetComputeRootShaderResourceView (SlotPositions, positionBuffer->GetGPUVirtualAddress ());
        list->SetComputeRootShaderResourceView (SlotRays, rayBuffer->GetGPUVirtualAddress ());
        list->SetComputeRootShaderResourceView (SlotDirections, directionBuffer->GetGPUVirtualAddress ());
        list->SetComputeRootUnorderedAccessView (SlotOccluded, outBuffer->GetGPUVirtualAddress ());
        uint32_t constants[4];
        std::memcpy (&constants[0], &tmin, sizeof (float));
        std::memcpy (&constants[1], &tmax, sizeof (float));
        constants[2] = static_cast<uint32_t> (base);
        constants[3] = count;
        list->SetComputeRoot32BitConstants (SlotLaunch, 4, constants, 0);
        list->SetPipelineState1 (pipeline_.Get ());
        D3D12_DISPATCH_RAYS_DESC dispatch {};
        dispatch.RayGenerationShaderRecord = { table, kRecordStride };
        dispatch.MissShaderTable = { table + kTableAlign, kRecordStride, kRecordStride };
        dispatch.HitGroupTable = { table + 2 * kTableAlign, kRecordStride, kRecordStride };
        dispatch.Width = count;
        dispatch.Height = 1;
        dispatch.Depth = 1;
        list->DispatchRays (&dispatch);
        hr = device_->Submit (options_.submitTimeoutMs);
        if (FAILED (hr))
            return device_->ReasonOf (hr, "DispatchRays", error);
    }

    hr = device_->Begin ();
    if (SUCCEEDED (hr)) {
        // Buffers decay to COMMON when a submission completes and promote
        // implicitly, so the copy needs no explicit transition.
        device_->List ()->CopyBufferRegion (readback.Get (), 0, outBuffer.Get (), 0, outBytes);
        hr = device_->Submit (options_.submitTimeoutMs);
    }
    void* mapped = nullptr;
    const D3D12_RANGE whole { 0, static_cast<SIZE_T> (outBytes) };
    if (SUCCEEDED (hr))
        hr = readback->Map (0, &whole, &mapped);
    if (FAILED (hr))
        return device_->ReasonOf (hr, "readback", error);
    std::memcpy (occluded.data (), mapped, static_cast<size_t> (outBytes));
    const D3D12_RANGE noWrite { 0, 0 };
    readback->Unmap (0, &noWrite);
    return GpuReason::None;
}

GpuReason DxrBackend::Trace (const packet::Request& request, packet::Result& result, protocol::RunTimings& timings,
                             const std::function<bool ()>& isCancelled, std::string& error)
{
    if (device_ == nullptr) {
        error = "no DXR device: " + capability_.message;
        return capability_.reason;
    }
    const auto started = std::chrono::steady_clock::now ();
    const size_t samples = request.positions.size () / 3;
    const size_t totalSteps = static_cast<size_t> (request.totalSteps);
    const size_t wordsPerSample = (totalSteps + 63) / 64;
    result.sampleCount = samples;
    result.stepCount = request.totalSteps;
    result.bits.assign (samples * wordsPerSample, 0u);
    result.resolved.assign (totalSteps, 0u);

    // QueryEngine::Occluded's interval: tmin clamps at 0, tmax <= 0 is
    // unbounded, and an empty interval holds nothing (clear, not shadowed).
    const double minT = request.tmin > 0.0 ? request.tmin : 0.0;
    const double maxT = request.tmax > 0.0 ? request.tmax : std::numeric_limits<double>::max ();
    const bool emptyInterval = maxT <= minT;

    // Front-facing (sample, step) pairs, decided in double as the accumulator
    // decides them; the GPU only answers whether they are blocked.
    std::vector<uint32_t> rays;
    std::vector<float> directions;
    std::vector<size_t> traceable; // step slots that need the GPU
    rays.reserve (samples * 2);
    for (size_t i = 0; i < request.steps.size (); ++i) {
        const size_t step = request.steps[i];
        const double* d = &request.directions[i * 3];
        const double length = std::sqrt (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const bool alwaysClear = sceneTriangles_ == 0 || length <= 0.0 || emptyInterval;
        const uint32_t slot = static_cast<uint32_t> (directions.size () / 3);
        for (size_t s = 0; s < samples; ++s) {
            if (!request.normals.empty ()) {
                const double* n = &request.normals[s * 3];
                if (n[0] * d[0] + n[1] * d[1] + n[2] * d[2] <= 0.0)
                    continue; // facing away: self-shadowed, bit stays clear
            }
            if (alwaysClear)
                result.bits[s * wordsPerSample + step / 64] |= 1ull << (step % 64);
            else {
                rays.push_back (static_cast<uint32_t> (s));
                rays.push_back (slot);
            }
        }
        if (!alwaysClear) {
            for (int a = 0; a < 3; ++a)
                directions.push_back (static_cast<float> (d[a] / length));
            traceable.push_back (step);
        }
    }

    if (!rays.empty ()) {
        std::vector<float> positions (samples * 3);
        for (size_t s = 0; s < samples; ++s)
            for (int a = 0; a < 3; ++a)
                positions[s * 3 + a] = static_cast<float> (request.positions[s * 3 + a] - origin_[a]);
        const float tmin = static_cast<float> (minT);
        const float tmax = request.tmax > 0.0 ? static_cast<float> (maxT) : std::numeric_limits<float>::max ();
        std::vector<uint32_t> occluded;
        const GpuReason reason = Dispatch (positions, rays, directions, tmin, tmax, occluded, isCancelled, error);
        if (reason != GpuReason::None)
            return reason;
        for (size_t r = 0; r < occluded.size (); ++r) {
            if (occluded[r] > 1) {
                error = "the GPU wrote an occlusion word that is neither 0 nor 1";
                return GpuReason::ValidationFailed;
            }
            if (occluded[r] != 0)
                continue;
            const size_t s = rays[r * 2];
            const size_t step = traceable[rays[r * 2 + 1]];
            result.bits[s * wordsPerSample + step / 64] |= 1ull << (step % 64);
        }
    }
    if (isCancelled && isCancelled ()) {
        error = "the run was cancelled";
        return GpuReason::WorkerFailed;
    }
    for (const uint32_t step : request.steps)
        result.resolved[step] = 1;
    timings.trace = Microseconds (started);
    return GpuReason::None;
}

} // namespace

std::unique_ptr<rayworker::RayBackend> MakeDxrBackend (const DxrBackendOptions& options)
{
    return std::make_unique<DxrBackend> (options);
}

} // namespace evp::dxrworker
