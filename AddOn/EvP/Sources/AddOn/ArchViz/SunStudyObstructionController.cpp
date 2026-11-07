#include "ArchViz/SunStudyObstructionController.hpp"

#include "ArchViz/SunStudyObstructionGeometry.hpp"
#include "Geometry/MeshStore.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "SunStudy/SunStudyTaskWorker.hpp"

#include <optional>
#include <mutex>
#include <utility>

namespace geomsrv::archviz {

namespace {

struct RequestKey {
    std::string studyId;
    uint64_t revision = 0, snapshotId = 0, captureStamp = 0;
    std::vector<size_t> samples;
    SunObstructionMode mode = SunObstructionMode::Off;
    int step = -1;

    bool operator== (const RequestKey& other) const = default;
};

struct PendingRequest {
    RequestKey key;
    uint64_t generation = 0;
    std::shared_ptr<const evp::sunstudy::StudyRecord> record;
};

} // namespace

struct SunObstructionController::Impl {
    mutable std::mutex mutex;
    evp::sunstudy::SunStudyTaskWorker worker;
    std::optional<RequestKey> current;
    std::optional<PendingRequest> pending;
    std::shared_ptr<const SunObstructionFrame> latest;
    uint64_t generation = 0;
    bool stopping = false;

    void Clear ()
    {
        worker.Cancel ();
        std::lock_guard<std::mutex> lock (mutex);
        current.reset ();
        pending.reset ();
        latest.reset ();
    }

    void Pump ()
    {
        evp::sunstudy::StudyTaskCompletion completion;
        if (worker.Poll (completion) && !completion.error.empty ()) {
            std::lock_guard<std::mutex> lock (mutex);
            auto frame = std::make_shared<SunObstructionFrame> ();
            frame->generation = completion.runGeneration;
            frame->error = completion.error;
            if (current && generation == frame->generation)
                latest = std::move (frame);
        }
        if (worker.Busy ())
            return;
        std::optional<PendingRequest> request;
        {
            std::lock_guard<std::mutex> lock (mutex);
            if (stopping || !pending)
                return;
            request = std::move (pending);
            pending.reset ();
        }
        const auto job = std::make_shared<PendingRequest> (std::move (*request));
        evp::sunstudy::StudyTaskRequest task;
        task.runGeneration = job->generation;
        task.execute = [this, job] (const std::atomic<bool>& cancelled) {
            evp::sunstudy::SunObstructionDayResult day;
            bool success = true;
            if (job->key.mode == SunObstructionMode::WholeDay)
                success = evp::sunstudy::AttributeSunObstructionDay (*job->record, job->key.samples, day,
                                                                     [&] { return cancelled.load (); });
            else {
                day.steps.emplace_back ();
                success = evp::sunstudy::AttributeSunObstruction (
                    *job->record, job->key.samples, job->record->series.Steps ()[size_t (job->key.step)], day.steps[0]);
            }
            if (cancelled.load ())
                return;
            if (!success && day.error.empty () && !day.steps.empty ())
                day.error = day.steps[0].error;
            if (!success && day.error.empty ())
                return;
            auto frame = std::make_shared<SunObstructionFrame> ();
            frame->generation = job->generation;
            frame->studyId = job->key.studyId;
            frame->snapshotId = job->key.snapshotId;
            frame->captureStamp = job->key.captureStamp;
            frame->mode = job->key.mode;
            frame->error = day.error;
            if (success) {
                SunObstructionGeometry geometry;
                success = BuildSunObstructionGeometry (*job->record->snapshot, day.steps,
                                                       job->key.mode == SunObstructionMode::WholeDay, geometry);
                frame->error = geometry.error;
                if (success) {
                    frame->drawables = std::move (geometry.drawables);
                    frame->blockerObjects = geometry.blockerObjects;
                    frame->timedParts = geometry.timedParts;
                    if (job->key.mode == SunObstructionMode::TimeStep && !day.steps.empty ())
                        frame->minute = day.steps[0].minute;
                }
            }
            std::lock_guard<std::mutex> lock (mutex);
            if (!cancelled.load () && current == job->key && generation == job->generation)
                latest = std::move (frame);
        };
        std::string error;
        if (!worker.Submit (std::move (task), error)) {
            std::lock_guard<std::mutex> lock (mutex);
            auto frame = std::make_shared<SunObstructionFrame> ();
            frame->generation = job->generation;
            frame->studyId = job->key.studyId;
            frame->error = error;
            if (current == job->key && generation == job->generation)
                latest = std::move (frame);
        }
    }
};

SunObstructionController::SunObstructionController () : impl_ (std::make_unique<Impl> ())
{
}

SunObstructionController::~SunObstructionController ()
{
    Shutdown ();
}

void SunObstructionController::Request (const SunObstructionRequest& request)
{
    if (request.mode == SunObstructionMode::Off || request.studyId.empty () || request.samples.empty ()) {
        impl_->Clear ();
        return;
    }
    const auto record = evp::sunstudy::SunStudyStore::Get ().CompletedRecord (request.studyId);
    if (record == nullptr || record->snapshot == nullptr) {
        impl_->Clear ();
        return;
    }
    const auto& steps = record->series.Steps ();
    if (request.mode == SunObstructionMode::TimeStep && (request.step < 0 || size_t (request.step) >= steps.size ())) {
        impl_->Clear ();
        return;
    }
    RequestKey key { request.studyId,
                     record->storeRevision,
                     record->snapshotId != 0 ? record->snapshotId : record->snapshot->id,
                     record->snapshot->captureStamp,
                     request.samples,
                     request.mode,
                     request.mode == SunObstructionMode::TimeStep ? request.step : -1 };
    {
        std::lock_guard<std::mutex> lock (impl_->mutex);
        if (impl_->stopping || (impl_->current == key && (impl_->pending || impl_->latest || impl_->worker.Busy ())))
            return;
        impl_->worker.Cancel ();
        impl_->latest.reset ();
        impl_->current = key;
        impl_->pending = PendingRequest { key, ++impl_->generation, record };
    }
    impl_->Pump ();
}

std::shared_ptr<const SunObstructionFrame> SunObstructionController::Latest ()
{
    impl_->Pump ();
    std::lock_guard<std::mutex> lock (impl_->mutex);
    return impl_->latest;
}

bool SunObstructionController::Busy () const
{
    std::lock_guard<std::mutex> lock (impl_->mutex);
    return impl_->pending.has_value () || impl_->worker.Busy ();
}

void SunObstructionController::Cancel ()
{
    impl_->Clear ();
}

void SunObstructionController::Shutdown ()
{
    {
        std::lock_guard<std::mutex> lock (impl_->mutex);
        if (impl_->stopping)
            return;
        impl_->stopping = true;
        impl_->pending.reset ();
        impl_->current.reset ();
        impl_->latest.reset ();
    }
    impl_->worker.Shutdown ();
}

SunObstructionController& SunObstructionTasks ()
{
    static SunObstructionController controller;
    return controller;
}

void ShutdownSunObstructionTasks ()
{
    SunObstructionTasks ().Shutdown ();
}

void ResetSunObstructionTasks ()
{
    SunObstructionTasks ().Cancel ();
}

bool SunObstructionSnapshotCurrent (const SunObstructionFrame& frame,
                                    const std::shared_ptr<const geomsrv::Snapshot>& shared, uint64_t captureStamp)
{
    return shared != nullptr && frame.snapshotId != 0 && shared->id == frame.snapshotId &&
           shared->captureStamp == frame.captureStamp && captureStamp == frame.captureStamp;
}

bool SunObstructionFrameCurrent (const std::shared_ptr<const SunObstructionFrame>& frame,
                                 const std::string& shownStudyId)
{
    if (frame == nullptr || frame->studyId != shownStudyId)
        return false;
    return SunObstructionSnapshotCurrent (*frame, MeshStore::Get ().Shared (), MeshStore::Get ().CaptureStamp ());
}

} // namespace geomsrv::archviz
