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
    int fromMinute = 0, toMinute = 1440;

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
        {
            std::lock_guard<std::mutex> lock (mutex);
            if (!current && !pending && !latest && !worker.Busy ())
                return; // the viewport calls Off every frame; an empty clear is free
        }
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
            const int divisions = evp::sunstudy::SunObstructionFootprintDivisions (job->key.samples.size ());
            if (job->key.mode == SunObstructionMode::WholeDay)
                success = evp::sunstudy::AttributeSunObstructionDay (
                    *job->record, job->key.samples, day, [&] { return cancelled.load (); }, divisions,
                    job->key.fromMinute, job->key.toMinute);
            else {
                day.steps.emplace_back ();
                success = evp::sunstudy::AttributeSunObstruction (*job->record, job->key.samples,
                                                                  job->record->series.Steps ()[size_t (job->key.step)],
                                                                  day.steps[0], divisions);
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
            const auto& record = *job->record;
            frame->cells = job->key.samples.size ();
            frame->fromMinute = job->key.fromMinute;
            frame->toMinute = job->key.toMinute;
            frame->windowFrom = record.hourFrom * 60;
            frame->windowTo = record.hourTo * 60;
            frame->timestepMinutes = record.timestepMinutes;
            frame->year = record.year;
            frame->month = record.month;
            frame->day = record.day;
            frame->footprintDivisions = divisions;
            frame->attributedSteps = day.steps.size ();
            if (!day.steps.empty ()) {
                frame->firstMinute = day.steps.front ().minute;
                frame->lastMinute = day.steps.back ().minute;
            }
            if (success) {
                SunObstructionGeometry geometry;
                success = BuildSunObstructionGeometry (*record.snapshot, day.steps, record.timestepMinutes, geometry);
                frame->error = geometry.error;
                if (success) {
                    frame->drawables = std::move (geometry.drawables);
                    frame->blockerObjects = geometry.blockerObjects;
                    frame->footprints = geometry.footprints;
                    frame->lit = geometry.lit;
                    frame->blocked = geometry.blocked;
                    frame->backFacing = geometry.backFacing;
                    frame->unavailable = geometry.unavailable;
                    for (const auto& blocker : geometry.blockers)
                        frame->blockerGuids.push_back (blocker.guid);
                    frame->blockers = std::move (geometry.blockers);
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
                     request.mode == SunObstructionMode::TimeStep ? request.step : -1,
                     request.mode == SunObstructionMode::WholeDay ? request.fromMinute : 0,
                     request.mode == SunObstructionMode::WholeDay ? request.toMinute : 1440 };
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
