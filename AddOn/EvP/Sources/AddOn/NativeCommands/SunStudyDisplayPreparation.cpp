#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/SunStudyDisplayPreparation.hpp"
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/ScenePacketTrace.hpp"
#include "Geometry/MeshStore.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "SunStudy/SunStudyTaskWorker.hpp"
#include "Python/MainThreadGate.hpp"

namespace geomsrv {
namespace {
archviz::SunStudyDisplayAssembler s_assembler;
std::mutex s_manualMutex;
uint64_t s_manualGeneration = 1;
ManualSunStudyDisplayState s_manualState;
std::shared_ptr<PreparedSunStudyDisplay> s_manualResult;

evp::sunstudy::SunStudyTaskWorker& ManualDisplayWorker ()
{
    // Join explicitly on unload, never from static destruction under loader lock.
    static auto* worker = new evp::sunstudy::SunStudyTaskWorker ();
    return *worker;
}

void PublishManualDisplay (uint64_t generation, const ManualSunStudyDisplayRequest& request)
{
    evp::sunstudy::StudyTaskCompletion completion;
    std::shared_ptr<PreparedSunStudyDisplay> result;
    {
        std::lock_guard<std::mutex> lock (s_manualMutex);
        if (generation != s_manualGeneration || !ManualDisplayWorker ().Poll (completion))
            return;
        result = std::move (s_manualResult);
        s_manualState.preparing = false;
        s_manualState.error = completion.error.empty () ? result->error : completion.error;
    }
    const bool published = sunfollow::PublishInSession (request.sessionGeneration, request.config, [&] {
        std::lock_guard<std::mutex> lock (s_manualMutex);
        const auto snapshot = MeshStore::Get ().Current ();
        const bool current =
            generation == s_manualGeneration && snapshot != nullptr && snapshot->id == request.snapshot->id &&
            evp::sunstudy::SunStudyStore::Get ().Revision (request.studyId) == request.revision &&
            (!MeshStore::Get ().CaptureActive () || MeshStore::Get ().CaptureStamp () == request.captureStamp) &&
            archviz::DiligentViewport::Get ().IsRunning ();
        if (!current || !completion.error.empty () || result->upload == nullptr)
            return false;
        return evp::sunstudy::SunStudyStore::Get ().PublishDisplayRecord (request.studyId, request.revision, [&] {
            archviz::SceneCmdQueue::Get ().PushSunStudyAtlas (std::move (result->upload));
        });
    });
    if (!published) {
        std::lock_guard<std::mutex> lock (s_manualMutex);
        if (generation == s_manualGeneration)
            s_manualState.error =
                !completion.error.empty ()
                    ? completion.error
                    : (!result->error.empty () ? result->error : "sun study display superseded or viewer closed");
        return; // retain previous overlay; a stale completion never clears it
    }
    if (request.follow)
        sunfollow::Adopt (request.studyId, request.config, request.sessionGeneration);
}
} // namespace

void PrepareSunStudyDisplay (const std::string& id, uint64_t revision, std::shared_ptr<const Snapshot> snapshot,
                             const archviz::SunStudyDisplayOptions& options, PreparedSunStudyDisplay& result,
                             const std::atomic<bool>& cancelled)
{
    const auto started = std::chrono::steady_clock::now ();
    if (snapshot == nullptr) {
        result.error = "sun study display has no captured snapshot";
        return;
    }
    std::string error;
    const bool read = evp::sunstudy::SunStudyStore::Get ().ReadDisplayRecord (
        id, revision,
        [&] (const evp::sunstudy::StudyRecord& record) {
            result.upload =
                s_assembler.Prepare (record, *snapshot, options, result.error, [&] { return cancelled.load (); });
        },
        error, &cancelled);
    if (!read) {
        result.upload.reset ();
        result.error = error;
    }
    if (result.upload != nullptr)
        archviz::LogSunStudyDisplay (
            *result.upload, snapshot->id,
            std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ());
    else if (result.error.empty ())
        result.error = "sun study display cancelled";
}

bool SubmitManualSunStudyDisplay (ManualSunStudyDisplayRequest request, std::string& error)
{
    std::lock_guard<std::mutex> lock (s_manualMutex);
    if (ManualDisplayWorker ().Busy ()) {
        error = "sun study display preparation is already running; retry after it completes";
        return false;
    }
    const uint64_t generation = ++s_manualGeneration;
    auto output = std::make_shared<PreparedSunStudyDisplay> ();
    evp::sunstudy::StudyTaskRequest task;
    task.sessionGeneration = request.sessionGeneration;
    task.runGeneration = generation;
    task.execute = [request, output] (const std::atomic<bool>& cancelled) {
        PrepareSunStudyDisplay (request.studyId, request.revision, request.snapshot, request.options, *output,
                                cancelled);
    };
    task.discard = [output] { output->upload.reset (); };
    task.onReady = [request, generation] {
        GS::UniString gateError;
        if (!evp::MainThreadGate::Get ().Post ([request, generation] { PublishManualDisplay (generation, request); },
                                               gateError)) {
            std::lock_guard<std::mutex> lock (s_manualMutex);
            if (generation == s_manualGeneration) {
                s_manualState.preparing = false;
                s_manualState.error = "host refused the display completion wake";
                ManualDisplayWorker ().Cancel ();
            }
        }
    };
    if (!ManualDisplayWorker ().Submit (std::move (task), error))
        return false;
    s_manualResult = output;
    s_manualState = { true, request.studyId, {} };
    return true;
}

ManualSunStudyDisplayState ManualSunStudyDisplayStatus ()
{
    std::lock_guard<std::mutex> lock (s_manualMutex);
    return s_manualState;
}

void CancelManualSunStudyDisplays ()
{
    std::lock_guard<std::mutex> lock (s_manualMutex);
    ++s_manualGeneration;
    ManualDisplayWorker ().Cancel ();
    s_manualResult.reset ();
    s_manualState = {};
}

void ShutdownManualSunStudyDisplays ()
{
    CancelManualSunStudyDisplays ();
    ManualDisplayWorker ().Shutdown ();
}

} // namespace geomsrv
