#include "RayWorker/RayWorkerTraversal.hpp"

#include "Geometry/QueryEngine.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace evp::rayworker {

namespace {

int64_t NowMs ()
{
    using namespace std::chrono;
    return duration_cast<milliseconds> (steady_clock::now ().time_since_epoch ()).count ();
}

// A step of a large model is a few hundred milliseconds of tracing; this is
// the "the worker is wedged" bound, not a performance budget.
constexpr int64_t kRunTimeoutMs = 120000;

std::atomic<uint64_t> gNextEpoch { 1 };

std::mutex gChannelMutex;
std::shared_ptr<RayWorkerChannel> gDxrChannel;

std::wstring BesideThisModule (const wchar_t* fileName)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR> (&BesideThisModule), &module))
        return fileName;
    wchar_t path[MAX_PATH] = {};
    const DWORD size = GetModuleFileNameW (module, path, MAX_PATH);
    std::wstring folder (path, size);
    const size_t slash = folder.find_last_of (L"\\/");
    return (slash == std::wstring::npos ? std::wstring () : folder.substr (0, slash + 1)) + fileName;
}

} // namespace

// ---- channel ----------------------------------------------------------------

RayWorkerChannel::RayWorkerChannel (std::wstring executable, win32::WorkerProcess::InProcessWorker inProcess)
    : host_ (std::move (executable), std::move (inProcess)), client_ (host_.Segments (), host_.Hooks ())
{
}

RayWorkerChannel::~RayWorkerChannel ()
{
    Shutdown ();
}

RayWorkerOutcome RayWorkerChannel::Run (uint64_t sceneEpoch, std::shared_ptr<const packet::Scene> scene,
                                        packet::Request request, const std::function<bool ()>& isCancelled,
                                        int64_t timeoutMs)
{
    std::lock_guard<std::mutex> lock (mutex_);
    const uint64_t generation = ++generation_;
    client_.Submit (generation, sceneEpoch, std::move (scene), std::move (request), NowMs ());
    const int64_t deadline = NowMs () + timeoutMs;
    for (;;) {
        auto events = host_.Take ();
        for (const auto& frame : events.frames)
            client_.OnFrame (frame, NowMs ());
        if (events.exited)
            client_.OnProcessExited (NowMs ());
        client_.Tick (NowMs ());
        for (auto& outcome : client_.TakeOutcomes ())
            if (outcome.generation == generation)
                return outcome;
        if (isCancelled && isCancelled ()) {
            // The run stays in flight; the next Submit supersedes it and its
            // answer is discarded unpublished.
            RayWorkerOutcome cancelled;
            cancelled.generation = generation;
            cancelled.superseded = true;
            cancelled.message = "cancelled";
            return cancelled;
        }
        if (NowMs () > deadline) {
            // Wedged rather than dead (it still heartbeats): stop it, so the
            // next run starts a fresh worker.
            client_.Shutdown ();
            host_.Kill ();
            RayWorkerOutcome timedOut;
            timedOut.generation = generation;
            timedOut.reason = GpuReason::WorkerTimeout;
            timedOut.message = "no answer within " + std::to_string (timeoutMs) + " ms";
            return timedOut;
        }
        std::this_thread::sleep_for (std::chrono::microseconds (200));
    }
}

void RayWorkerChannel::Shutdown ()
{
    std::lock_guard<std::mutex> lock (mutex_);
    client_.Shutdown ();
    host_.Kill ();
}

std::string RayWorkerChannel::Describe ()
{
    std::lock_guard<std::mutex> lock (mutex_);
    return client_.Supervisor ().Describe ();
}

// ---- traversal --------------------------------------------------------------

RayWorkerTraversal::RayWorkerTraversal (std::shared_ptr<const sunstudy::ITraversal> cpu,
                                        std::shared_ptr<RayWorkerChannel> channel,
                                        std::shared_ptr<const packet::Scene> scene, std::string backendName,
                                        std::function<void (const std::string&)> log)
    : cpu_ (std::move (cpu)), channel_ (std::move (channel)), scene_ (std::move (scene)),
      backendName_ (std::move (backendName)), log_ (std::move (log)), epoch_ (gNextEpoch.fetch_add (1))
{
    if (channel_ == nullptr || scene_ == nullptr)
        onCpu_.store (true);
}

void RayWorkerTraversal::OccludeDirectional (const double* origins, size_t count, const double dir[3], double tmin,
                                             double tmax, uint8_t* out, size_t maxParallel) const
{
    OccludeDirectionalCancellable (origins, count, dir, tmin, tmax, out, maxParallel, {});
}

bool RayWorkerTraversal::OccludeDirectionalCancellable (const double* origins, size_t count, const double dir[3],
                                                        double tmin, double tmax, uint8_t* out, size_t maxParallel,
                                                        const std::function<bool ()>& isCancelled) const
{
    if (onCpu_.load () || count == 0 || origins == nullptr || dir == nullptr || out == nullptr)
        return cpu_->OccludeDirectionalCancellable (origins, count, dir, tmin, tmax, out, maxParallel, isCancelled);

    packet::Request request;
    request.tmin = tmin;
    request.tmax = tmax;
    request.totalSteps = 1;
    request.positions.assign (origins, origins + count * 3);
    request.steps = { 0 };
    request.directions = { dir[0], dir[1], dir[2] };
    const RayWorkerOutcome outcome = channel_->Run (epoch_, scene_, std::move (request), isCancelled, kRunTimeoutMs);
    if (outcome.published && outcome.result.sampleCount == count && outcome.result.bits.size () == count &&
        outcome.result.resolved.size () == 1 && outcome.result.resolved[0] == 1) {
        // One step, so one word per sample; bit 0 is "lit at step 0".
        for (size_t i = 0; i < count; ++i)
            out[i] = (outcome.result.bits[i] & 1u) != 0 ? uint8_t { 0 } : uint8_t { 1 };
        gpuCalls_.fetch_add (1);
        return true;
    }
    if (outcome.superseded && !outcome.published && outcome.message == "cancelled")
        return false; // the caller cancelled; nothing to fall back from

    const GpuReason reason = outcome.published
                                 ? GpuReason::ValidationFailed
                                 : (outcome.reason == GpuReason::None ? GpuReason::WorkerFailed : outcome.reason);
    if (!onCpu_.exchange (true)) {
        reason_.store (static_cast<uint32_t> (reason));
        if (log_)
            log_ ("pipeline: stage=sun-traversal requested=" + backendName_ +
                  " actual=cpu reason=" + GpuReasonCode (reason) + " gpuSteps=" + std::to_string (gpuCalls_.load ()) +
                  " detail=" + outcome.message);
    }
    return cpu_->OccludeDirectionalCancellable (origins, count, dir, tmin, tmax, out, maxParallel, isCancelled);
}

void RayWorkerTraversal::OccludeRays (const sunstudy::OcclusionRay* rays, size_t count, uint8_t* out,
                                      size_t maxParallel) const
{
    cpu_->OccludeRays (rays, count, out, maxParallel);
}

uint64_t RayWorkerTraversal::SceneVersion () const
{
    return cpu_->SceneVersion ();
}

std::string RayWorkerTraversal::ActualBackend () const
{
    return onCpu_.load () ? "cpu" : backendName_;
}

GpuReason RayWorkerTraversal::FallbackReason () const
{
    return static_cast<GpuReason> (reason_.load ());
}

// ---- selection and the add-on's channel ------------------------------------

bool DxrTraversalRequested ()
{
    const char* value = std::getenv ("TAPIOCA_SUNSTUDY_TRAVERSAL");
    if (value == nullptr)
        return false;
    std::string text (value);
    std::transform (text.begin (), text.end (), text.begin (),
                    [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return text == "dxr";
}

std::shared_ptr<const packet::Scene> SceneFromIndices (const geomsrv::QueryEngine* analysis,
                                                       const geomsrv::QueryEngine* context)
{
    auto scene = std::make_shared<packet::Scene> ();
    for (const geomsrv::QueryEngine* index : { analysis, context }) {
        if (index == nullptr || index->SourceSnapshot () == nullptr)
            continue;
        const packet::Scene part = packet::SceneFromSnapshot (*index->SourceSnapshot ());
        const uint32_t vertexBase = static_cast<uint32_t> (scene->vertices.size () / 3);
        const uint32_t meshBase = scene->meshCount;
        scene->vertices.insert (scene->vertices.end (), part.vertices.begin (), part.vertices.end ());
        for (const uint32_t v : part.triangles)
            scene->triangles.push_back (vertexBase + v);
        for (const uint32_t m : part.triangleMesh)
            scene->triangleMesh.push_back (meshBase + m);
        scene->meshCount += part.meshCount;
    }
    return scene;
}

std::shared_ptr<RayWorkerChannel> DxrWorkerChannel ()
{
    std::lock_guard<std::mutex> lock (gChannelMutex);
    if (gDxrChannel == nullptr)
        gDxrChannel = std::make_shared<RayWorkerChannel> (BesideThisModule (L"TapiocaDxrWorker.exe"));
    return gDxrChannel;
}

void ShutdownDxrWorker ()
{
    std::shared_ptr<RayWorkerChannel> channel;
    {
        std::lock_guard<std::mutex> lock (gChannelMutex);
        channel.swap (gDxrChannel);
    }
    if (channel != nullptr)
        channel->Shutdown ();
}

} // namespace evp::rayworker
