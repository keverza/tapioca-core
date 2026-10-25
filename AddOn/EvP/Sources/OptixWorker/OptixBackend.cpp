#include "OptixWorker/OptixBackend.hpp"

#include "OptixWorker/OptixLaunchParams.h"
#include "RayWorker/RayWorkerBatch.hpp"

#include <cuda.h>
#include <optix.h>
#include <optix_function_table_definition.h> // exactly one TU defines the table
#include <optix_stubs.h>

// Generated at build time from OptixOcclusion.cu (nvcc -optix-ir); not committed.
#include "OptixOcclusionIr.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace evp::optixworker {

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

std::string CudaText (CUresult result)
{
    const char* name = nullptr;
    cuGetErrorName (result, &name);
    return name != nullptr ? name : ("CUresult " + std::to_string (static_cast<int> (result)));
}

GpuReason CudaReason (CUresult result, const char* what, std::string& error)
{
    error = std::string (what) + " failed: " + CudaText (result);
    if (result == CUDA_ERROR_OUT_OF_MEMORY)
        return GpuReason::OutOfMemory;
    if (result == CUDA_ERROR_LAUNCH_TIMEOUT || result == CUDA_ERROR_LAUNCH_FAILED ||
        result == CUDA_ERROR_ILLEGAL_ADDRESS)
        return GpuReason::DeviceRemoved; // the context is lost; nothing on it can be trusted
    return GpuReason::WorkerFailed;
}

GpuReason OptixReason (OptixResult result, const char* what, std::string& error)
{
    error = std::string (what) + " failed: " + optixGetErrorName (result);
    if (result == OPTIX_ERROR_CUDA_ERROR)
        return GpuReason::DeviceRemoved;
    return GpuReason::WorkerFailed;
}

// One device allocation, freed with its owner.
struct DeviceBuffer {
    CUdeviceptr ptr = 0;
    size_t bytes = 0;
    ~DeviceBuffer ()
    {
        Free ();
    }
    void Free ()
    {
        if (ptr != 0)
            cuMemFree (ptr);
        ptr = 0;
        bytes = 0;
    }
    CUresult Alloc (size_t size)
    {
        Free ();
        bytes = size;
        return cuMemAlloc (&ptr, std::max<size_t> (size, 4));
    }
    CUresult Upload (const void* data, size_t size)
    {
        CUresult result = Alloc (size);
        if (result == CUDA_SUCCESS && size > 0)
            result = cuMemcpyHtoD (ptr, data, size);
        return result;
    }
};

struct SbtRecord {
    alignas (OPTIX_SBT_RECORD_ALIGNMENT) char header[OPTIX_SBT_RECORD_HEADER_SIZE];
};

class OptixBackend final : public rayworker::RayBackend {
  public:
    explicit OptixBackend (const OptixBackendOptions& options) : options_ (options)
    {
        Initialise ();
    }

    ~OptixBackend () override
    {
        gas_.Free ();
        sbt_.Free ();
        params_.Free ();
        if (pipeline_ != nullptr)
            optixPipelineDestroy (pipeline_);
        for (OptixProgramGroup group : groups_)
            if (group != nullptr)
                optixProgramGroupDestroy (group);
        if (module_ != nullptr)
            optixModuleDestroy (module_);
        if (optix_ != nullptr)
            optixDeviceContextDestroy (optix_);
        if (stream_ != nullptr)
            cuStreamDestroy (stream_);
        if (cuda_ != nullptr)
            cuDevicePrimaryCtxRelease (device_);
    }

    protocol::HelloPayload Describe () const override
    {
        protocol::HelloPayload hello;
        hello.reason = reason_;
        if (reason_ == GpuReason::None)
            hello.capabilities = protocol::CapabilityHardwareRayTracing;
        hello.driverVersion = driverVersion_;
        hello.runtimeVersion = OPTIX_VERSION; // e.g. 90100
        hello.computeCapability = computeCapability_;
        hello.deviceMemoryBytes = memoryBytes_;
        hello.deviceName = reason_ == GpuReason::None ? deviceName_ : message_;
        return hello;
    }

    GpuReason BuildScene (const packet::Scene& scene, protocol::SceneReadyPayload& ready, std::string& error) override
    {
        const auto started = std::chrono::steady_clock::now ();
        gas_.Free ();
        gasHandle_ = 0;
        sceneTriangles_ = 0;
        if (reason_ != GpuReason::None) {
            error = "no OptiX device: " + message_;
            return reason_;
        }
        if (!rayworker::SceneOrigin (scene, origin_)) {
            error = "the scene has a non-finite vertex";
            return GpuReason::ValidationFailed;
        }
        const size_t triangleCount = scene.triangles.size () / 3;
        if (triangleCount == 0) {
            ready.buildMicroseconds = Microseconds (started);
            return GpuReason::None; // nothing occludes: every traced ray is clear
        }
        cuCtxSetCurrent (cuda_);
        const std::vector<float> local = rayworker::LocalVertices (scene, origin_);
        DeviceBuffer vertices, indices, temp;
        CUresult cu = vertices.Upload (local.data (), local.size () * sizeof (float));
        if (cu == CUDA_SUCCESS)
            cu = indices.Upload (scene.triangles.data (), scene.triangles.size () * sizeof (uint32_t));
        if (cu != CUDA_SUCCESS)
            return CudaReason (cu, "scene upload", error);

        const unsigned int flags[1] = { OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT };
        OptixBuildInput input {};
        input.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
        input.triangleArray.vertexBuffers = &vertices.ptr;
        input.triangleArray.numVertices = static_cast<unsigned int> (local.size () / 3);
        input.triangleArray.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
        input.triangleArray.vertexStrideInBytes = 3 * sizeof (float);
        input.triangleArray.indexBuffer = indices.ptr;
        input.triangleArray.numIndexTriplets = static_cast<unsigned int> (triangleCount);
        input.triangleArray.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
        input.triangleArray.indexStrideInBytes = 3 * sizeof (uint32_t);
        input.triangleArray.flags = flags;
        input.triangleArray.numSbtRecords = 1;

        OptixAccelBuildOptions build {};
        build.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
        build.operation = OPTIX_BUILD_OPERATION_BUILD;
        OptixAccelBufferSizes sizes {};
        OptixResult ox = optixAccelComputeMemoryUsage (optix_, &build, &input, 1, &sizes);
        if (ox != OPTIX_SUCCESS)
            return OptixReason (ox, "optixAccelComputeMemoryUsage", error);
        cu = temp.Alloc (sizes.tempSizeInBytes);
        if (cu == CUDA_SUCCESS)
            cu = gas_.Alloc (sizes.outputSizeInBytes);
        if (cu != CUDA_SUCCESS)
            return CudaReason (cu, "acceleration structure allocation", error);
        ox = optixAccelBuild (optix_, stream_, &build, &input, 1, temp.ptr, temp.bytes, gas_.ptr, gas_.bytes,
                              &gasHandle_, nullptr, 0);
        if (ox != OPTIX_SUCCESS)
            return OptixReason (ox, "optixAccelBuild", error);
        // The vertex and index buffers are freed on return: a triangle GAS keeps
        // no reference to its build inputs, so the build must finish first.
        cu = cuStreamSynchronize (stream_);
        if (cu != CUDA_SUCCESS)
            return CudaReason (cu, "acceleration structure build", error);
        sceneTriangles_ = triangleCount;
        ready.buildMicroseconds = Microseconds (started);
        ready.deviceBytes = sizes.outputSizeInBytes;
        return GpuReason::None;
    }

    GpuReason Trace (const packet::Request& request, packet::Result& result, protocol::RunTimings& timings,
                     const std::function<bool ()>& isCancelled, std::string& error) override
    {
        if (reason_ != GpuReason::None) {
            error = "no OptiX device: " + message_;
            return reason_;
        }
        const auto started = std::chrono::steady_clock::now ();
        const rayworker::RayBatch batch = rayworker::BuildRayBatch (request, sceneTriangles_, origin_, result);
        std::vector<uint32_t> occluded;
        if (batch.RayCount () > 0) {
            const GpuReason reason = Dispatch (batch, occluded, isCancelled, error);
            if (reason != GpuReason::None)
                return reason;
        }
        if (isCancelled && isCancelled ()) {
            error = "the run was cancelled";
            return GpuReason::WorkerFailed;
        }
        if (!rayworker::ApplyOcclusion (batch, occluded, request, result, error))
            return GpuReason::ValidationFailed;
        timings.trace = Microseconds (started);
        return GpuReason::None;
    }

  private:
    void Initialise ();
    bool CreatePipeline ();
    GpuReason Dispatch (const rayworker::RayBatch& batch, std::vector<uint32_t>& occluded,
                        const std::function<bool ()>& isCancelled, std::string& error);

    OptixBackendOptions options_;
    GpuReason reason_ = GpuReason::GpuUnsupported;
    std::string message_;
    std::string deviceName_;
    uint32_t driverVersion_ = 0;
    uint32_t computeCapability_ = 0;
    uint64_t memoryBytes_ = 0;

    CUcontext cuda_ = nullptr;
    CUdevice device_ = 0;
    CUstream stream_ = nullptr;
    OptixDeviceContext optix_ = nullptr;
    OptixModule module_ = nullptr;
    OptixProgramGroup groups_[3] = { nullptr, nullptr, nullptr }; // raygen, miss, hit
    OptixPipeline pipeline_ = nullptr;
    OptixShaderBindingTable table_ {};
    DeviceBuffer sbt_;
    DeviceBuffer params_;
    DeviceBuffer gas_;
    OptixTraversableHandle gasHandle_ = 0;
    size_t sceneTriangles_ = 0;
    double origin_[3] = { 0.0, 0.0, 0.0 };
};

void OptixBackend::Initialise ()
{
    CUresult cu = cuInit (0);
    int count = 0;
    if (cu == CUDA_SUCCESS)
        cu = cuDeviceGetCount (&count);
    if (cu != CUDA_SUCCESS || count == 0) {
        message_ = cu != CUDA_SUCCESS ? "CUDA unavailable: " + CudaText (cu) : "no CUDA device";
        reason_ = cu == CUDA_ERROR_SYSTEM_DRIVER_MISMATCH ? GpuReason::DriverIncompatible : GpuReason::GpuUnsupported;
        return;
    }
    // The first device with hardware ray-tracing cores (compute 7.5, Turing+);
    // OptiX runs on older ones in software, which never qualifies a machine.
    CUdevice device = -1;
    for (int i = 0; i < count; ++i) {
        CUdevice candidate = 0;
        int major = 0, minor = 0;
        cuDeviceGet (&candidate, i);
        cuDeviceGetAttribute (&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, candidate);
        cuDeviceGetAttribute (&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, candidate);
        if (major * 10 + minor >= 75) {
            device = candidate;
            computeCapability_ = static_cast<uint32_t> (major * 10 + minor);
            break;
        }
    }
    if (device < 0) {
        message_ = "no CUDA device with RT cores (compute capability 7.5+)";
        return;
    }
    char name[256] = {};
    cuDeviceGetName (name, sizeof (name), device);
    deviceName_ = name;
    size_t memory = 0;
    cuDeviceTotalMem (&memory, device);
    memoryBytes_ = memory;
    int driver = 0;
    cuDriverGetVersion (&driver); // CUDA API level of the driver, e.g. 13000
    driverVersion_ = static_cast<uint32_t> (driver);

    // The device's primary context: the same call on every CUDA version, and
    // the one any other CUDA user in this process would share.
    cu = cuDevicePrimaryCtxRetain (&cuda_, device);
    if (cu == CUDA_SUCCESS)
        device_ = device;
    if (cu == CUDA_SUCCESS)
        cu = cuCtxSetCurrent (cuda_);
    if (cu == CUDA_SUCCESS)
        cu = cuStreamCreate (&stream_, CU_STREAM_NON_BLOCKING);
    if (cu != CUDA_SUCCESS) {
        message_ = "CUDA context creation failed: " + CudaText (cu);
        return;
    }
    // Loads nvoptix.dll from the driver and checks its ABI against this build.
    const OptixResult ox = optixInit ();
    if (ox != OPTIX_SUCCESS) {
        message_ = std::string ("optixInit failed: ") + optixGetErrorName (ox);
        reason_ = ox == OPTIX_ERROR_UNSUPPORTED_ABI_VERSION || ox == OPTIX_ERROR_LIBRARY_NOT_FOUND
                      ? GpuReason::DriverIncompatible
                      : GpuReason::GpuUnsupported;
        return;
    }
    OptixDeviceContextOptions contextOptions {};
    if (optixDeviceContextCreate (cuda_, &contextOptions, &optix_) != OPTIX_SUCCESS || !CreatePipeline ()) {
        if (message_.empty ())
            message_ = "OptiX device context creation failed";
        // The driver's OptiX compiles our embedded IR; IR from a newer toolkit
        // than the driver knows is refused (cmake/OptixWorker.cmake).
        reason_ = message_.find ("NvvmIRVersion") != std::string::npos ||
                          message_.find ("version mismatch") != std::string::npos
                      ? GpuReason::DriverIncompatible
                      : GpuReason::WorkerFailed;
        return;
    }
    reason_ = GpuReason::None;
}

bool OptixBackend::CreatePipeline ()
{
    OptixModuleCompileOptions moduleOptions {};
    moduleOptions.optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
    moduleOptions.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_NONE;
    OptixPipelineCompileOptions pipelineOptions {};
    pipelineOptions.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS;
    pipelineOptions.numPayloadValues = 1;
    pipelineOptions.numAttributeValues = 2;
    pipelineOptions.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE;
    pipelineOptions.pipelineLaunchParamsVariableName = "params";
    pipelineOptions.usesPrimitiveTypeFlags = static_cast<unsigned int> (OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE);

    char log[2048];
    size_t logSize = sizeof (log);
    OptixResult ox =
        optixModuleCreate (optix_, &moduleOptions, &pipelineOptions, reinterpret_cast<const char*> (kOptixOcclusionIr),
                           sizeof (kOptixOcclusionIr), log, &logSize, &module_);
    if (ox != OPTIX_SUCCESS) {
        message_ = std::string ("optixModuleCreate failed: ") + optixGetErrorName (ox) + " " + log;
        return false;
    }

    OptixProgramGroupDesc descs[3] = {};
    descs[0].kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    descs[0].raygen.module = module_;
    descs[0].raygen.entryFunctionName = "__raygen__occlusion";
    descs[1].kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
    descs[1].miss.module = module_;
    descs[1].miss.entryFunctionName = "__miss__occlusion";
    // An empty hit group: geometry is opaque and closest-hit is disabled, so
    // a hit only terminates the trace.
    descs[2].kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
    OptixProgramGroupOptions groupOptions {};
    logSize = sizeof (log);
    ox = optixProgramGroupCreate (optix_, descs, 3, &groupOptions, log, &logSize, groups_);
    if (ox != OPTIX_SUCCESS) {
        message_ = std::string ("optixProgramGroupCreate failed: ") + optixGetErrorName (ox);
        return false;
    }
    OptixPipelineLinkOptions linkOptions {};
    linkOptions.maxTraceDepth = 1;
    logSize = sizeof (log);
    ox = optixPipelineCreate (optix_, &pipelineOptions, &linkOptions, groups_, 3, log, &logSize, &pipeline_);
    if (ox == OPTIX_SUCCESS)
        ox = optixPipelineSetStackSizeFromCallDepths (pipeline_, 1, 0, 0, 0, 1);
    if (ox != OPTIX_SUCCESS) {
        message_ = std::string ("optixPipelineCreate failed: ") + optixGetErrorName (ox);
        return false;
    }

    SbtRecord records[3];
    for (int i = 0; i < 3; ++i)
        if (optixSbtRecordPackHeader (groups_[i], &records[i]) != OPTIX_SUCCESS) {
            message_ = "optixSbtRecordPackHeader failed";
            return false;
        }
    if (sbt_.Upload (records, sizeof (records)) != CUDA_SUCCESS ||
        params_.Alloc (sizeof (LaunchParams)) != CUDA_SUCCESS) {
        message_ = "shader binding table allocation failed";
        return false;
    }
    table_.raygenRecord = sbt_.ptr;
    table_.missRecordBase = sbt_.ptr + sizeof (SbtRecord);
    table_.missRecordStrideInBytes = sizeof (SbtRecord);
    table_.missRecordCount = 1;
    table_.hitgroupRecordBase = sbt_.ptr + 2 * sizeof (SbtRecord);
    table_.hitgroupRecordStrideInBytes = sizeof (SbtRecord);
    table_.hitgroupRecordCount = 1;
    return true;
}

GpuReason OptixBackend::Dispatch (const rayworker::RayBatch& batch, std::vector<uint32_t>& occluded,
                                  const std::function<bool ()>& isCancelled, std::string& error)
{
    cuCtxSetCurrent (cuda_);
    const size_t rayCount = batch.RayCount ();
    DeviceBuffer positions, rays, directions, out;
    CUresult cu = positions.Upload (batch.positions.data (), batch.positions.size () * sizeof (float));
    if (cu == CUDA_SUCCESS)
        cu = rays.Upload (batch.rays.data (), batch.rays.size () * sizeof (uint32_t));
    if (cu == CUDA_SUCCESS)
        cu = directions.Upload (batch.directions.data (), batch.directions.size () * sizeof (float));
    if (cu == CUDA_SUCCESS)
        cu = out.Alloc (rayCount * sizeof (uint32_t));
    if (cu != CUDA_SUCCESS)
        return CudaReason (cu, "ray upload", error);

    LaunchParams params {};
    params.scene = gasHandle_;
    params.positions = reinterpret_cast<const float3*> (positions.ptr);
    params.rays = reinterpret_cast<const uint2*> (rays.ptr);
    params.directions = reinterpret_cast<const float3*> (directions.ptr);
    params.occluded = reinterpret_cast<unsigned int*> (out.ptr);
    params.tmin = batch.tmin;
    params.tmax = batch.tmax;
    for (size_t base = 0; base < rayCount; base += options_.raysPerLaunch) {
        // ⚠️ CANCELLATION BETWEEN LAUNCHES ONLY; a cancelled run commits nothing.
        if (isCancelled && isCancelled ()) {
            error = "the run was cancelled";
            return GpuReason::WorkerFailed;
        }
        params.rayBase = static_cast<unsigned int> (base);
        params.rayCount = static_cast<unsigned int> (std::min<size_t> (options_.raysPerLaunch, rayCount - base));
        cu = cuMemcpyHtoDAsync (params_.ptr, &params, sizeof (params), stream_);
        if (cu != CUDA_SUCCESS)
            return CudaReason (cu, "launch parameters", error);
        const OptixResult ox =
            optixLaunch (pipeline_, stream_, params_.ptr, sizeof (params), &table_, params.rayCount, 1, 1);
        if (ox != OPTIX_SUCCESS)
            return OptixReason (ox, "optixLaunch", error);
        cu = cuStreamSynchronize (stream_);
        if (cu != CUDA_SUCCESS)
            return CudaReason (cu, "launch", error);
    }
    occluded.resize (rayCount);
    cu = cuMemcpyDtoH (occluded.data (), out.ptr, rayCount * sizeof (uint32_t));
    if (cu != CUDA_SUCCESS)
        return CudaReason (cu, "readback", error);
    return GpuReason::None;
}

} // namespace

std::unique_ptr<rayworker::RayBackend> MakeOptixBackend (const OptixBackendOptions& options)
{
    return std::make_unique<OptixBackend> (options);
}

} // namespace evp::optixworker
