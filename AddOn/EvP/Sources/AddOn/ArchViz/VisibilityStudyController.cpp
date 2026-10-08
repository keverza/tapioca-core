#include "ArchViz/VisibilityStudyController.hpp"

#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/VisibilityStudyCapture.hpp"
#include "ArchViz/VisibilityStudyDisplay.hpp"
#include "Geometry/MeshStore.hpp"

#include <atomic>
#include <mutex>
#include <thread>

namespace geomsrv::archviz::visibilitystudy {

namespace {

class Worker final {
  public:
    ~Worker ()
    {
        Shutdown ();
    }

    bool Submit (Request request, std::string& error)
    {
        std::lock_guard<std::mutex> lifecycleLock (lifecycleMutex_);
        const uint64_t captureStamp = MeshStore::Get ().CaptureStamp ();
        if (!CaptureIsFresh (request.snapshot, captureStamp)) {
            error = "the viewer capture is missing or stale; wait for model extraction";
            return false;
        }
        std::thread previous;
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock (mutex_);
            if (status_.running) {
                error = "a visibility study is already running";
                return false;
            }
            previous = std::move (thread_);
            cancelled_.store (false);
            status_ = {};
            status_.running = true;
            status_.snapshotId = request.snapshot != nullptr ? request.snapshot->id : 0;
            generation = ++generation_;
            displayGeneration_ = SceneCmdQueue::Get ().ClaimAnalysisDisplay (1);
        }
        if (previous.joinable ())
            previous.join ();
        std::lock_guard<std::mutex> startLock (mutex_);
        const uint64_t displayGeneration = displayGeneration_;
        thread_ =
            std::thread ([this, request = std::move (request), generation, captureStamp, displayGeneration] () mutable {
                auto result =
                    evp::sunstudy::RunVisibilityStudy (request.snapshot, request.fromElements, request.toElements,
                                                       request.options, [this] { return cancelled_.load (); });
                result.id = "visibility-viewer-" + std::to_string (generation);
                std::string displayError;
                std::unique_ptr<SunStudyAtlasUpload> upload;
                if (result.valid)
                    upload = BuildVisibilityStudyUpload (result, displayError, [this] { return cancelled_.load (); });

                std::lock_guard<std::mutex> lock (mutex_);
                if (generation != generation_)
                    return;
                if (cancelled_.load ())
                    displayError = "visibility study cancelled";
                else if (!CaptureIsFresh (request.snapshot, captureStamp))
                    displayError = "the model changed while visibility was calculating; run it again";
                else if (result.valid && displayError.empty () && upload != nullptr) {
                    upload->captureStamp = captureStamp;
                    if (!SceneCmdQueue::Get ().PushAnalysisAtlas (displayGeneration, std::move (upload)))
                        displayError = "visibility display superseded or viewer closed";
                }
                status_.running = false;
                status_.complete = result.valid && displayError.empty ();
                status_.studyId = result.id;
                status_.error = !result.error.empty () ? result.error : displayError;
                status_.snapshotId = result.snapshotId;
                status_.sampleCount = result.values.size ();
                status_.aimPointCount = result.aimPointCount;
                status_.rayCount = result.rayCount;
                status_.visibleSamples = result.visibleSamples;
                status_.meanVisibility = result.meanVisibility;
                status_.analysisMilliseconds = result.analysisMilliseconds;
            });
        return true;
    }

    Status StatusOf () const
    {
        std::lock_guard<std::mutex> lock (mutex_);
        return status_;
    }

    void Cancel ()
    {
        std::lock_guard<std::mutex> lock (mutex_);
        cancelled_.store (true);
    }

    void ClearDisplay ()
    {
        std::lock_guard<std::mutex> lock (mutex_);
        cancelled_.store (true);
        SceneCmdQueue::Get ().PushClearAnalysisKind (1);
        status_.complete = false;
        status_.error.clear ();
    }

    void Shutdown ()
    {
        std::lock_guard<std::mutex> lifecycleLock (lifecycleMutex_);
        std::thread running;
        {
            std::lock_guard<std::mutex> lock (mutex_);
            cancelled_.store (true);
            ++generation_;
            running = std::move (thread_);
            status_ = {};
        }
        if (running.joinable ())
            running.join ();
    }

  private:
    std::mutex lifecycleMutex_;
    mutable std::mutex mutex_;
    std::thread thread_;
    std::atomic<bool> cancelled_ { false };
    Status status_;
    uint64_t generation_ = 0;
    uint64_t displayGeneration_ = 0;
};

Worker& GetWorker ()
{
    static Worker worker;
    return worker;
}

} // namespace

bool Submit (Request request, std::string& error)
{
    return GetWorker ().Submit (std::move (request), error);
}

Status GetStatus ()
{
    return GetWorker ().StatusOf ();
}

void Cancel ()
{
    GetWorker ().Cancel ();
}

void ClearDisplay ()
{
    GetWorker ().ClearDisplay ();
}

void Shutdown ()
{
    GetWorker ().Shutdown ();
}

} // namespace geomsrv::archviz::visibilitystudy
