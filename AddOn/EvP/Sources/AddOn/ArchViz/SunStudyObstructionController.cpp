#include "ArchViz/SunStudyObstructionController.hpp"

#include "ArchViz/SunStudyObstructionGeometry.hpp"
#include "Geometry/MeshStore.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "SunStudy/SunStudyOccluders.hpp"
#include "SunStudy/SunStudyPointProfile.hpp"
#include "SunStudy/SunStudyRoles.hpp"
#include "SunStudy/SunStudySolarCut.hpp"
#include "SunStudy/SunStudyTaskWorker.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
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
    bool lifted = false;
    double liftedPoint[3] = {};
    double radius = 0.0;

    bool operator== (const RequestKey& other) const = default;
};

struct PendingRequest {
    RequestKey key;
    uint64_t generation = 0;
    std::shared_ptr<const evp::sunstudy::StudyRecord> record;
};

} // namespace

// The solar cut for one request, on the worker: the point, the slice's suns,
// which elements may block, then the cut and its preview. Moving the point
// must answer at once, so the frame is published as it grows: the cutting
// surface (a few rays), then the cut (every blocker clipped), then the shadows
// re-traced against what remains (the scene's occluders rebuilt).
static std::shared_ptr<SunObstructionFrame>
SolarCutFrame (const PendingRequest& job, const std::atomic<bool>& cancelled,
               const std::function<void (const SunObstructionFrame&)>& publish)
{
    const auto& record = *job.record;
    auto frame = std::make_shared<SunObstructionFrame> ();
    frame->generation = job.generation;
    frame->studyId = job.key.studyId;
    frame->snapshotId = job.key.snapshotId;
    frame->captureStamp = job.key.captureStamp;
    frame->mode = SunObstructionMode::SolarCut;
    frame->fromMinute = job.key.fromMinute;
    frame->toMinute = job.key.toMinute;
    frame->windowFrom = record.hourFrom * 60;
    frame->windowTo = record.hourTo * 60;
    frame->timestepMinutes = record.timestepMinutes;
    frame->year = record.year;
    frame->month = record.month;
    frame->day = record.day;
    frame->cells = job.key.samples.size ();
    if (!record.defaultRayBounds) {
        frame->error = "Solar cut cannot reproduce this study's custom ray bounds; run with default ray bounds";
        return frame;
    }
    const size_t sample = job.key.samples.front ();
    if (!job.key.lifted && sample >= record.positions.size () / 3) {
        frame->error = "the study point's cell is outside the study";
        return frame;
    }
    evp::sunstudy::SolarCutInput input;
    if (job.key.lifted)
        std::copy (job.key.liftedPoint, job.key.liftedPoint + 3, input.point);
    else {
        std::copy (&record.positions[3 * sample], &record.positions[3 * sample] + 3, input.point);
        if (record.normals.size () == record.positions.size ())
            input.normal = &record.normals[3 * sample];
        if (sample < record.sampleMeshes.size ())
            input.ownMesh = record.sampleMeshes[sample];
    }
    for (const auto& step : record.series.Steps ()) {
        const int minute = step.time.hour * 60 + step.time.minute;
        if (minute >= job.key.fromMinute && minute < job.key.toMinute)
            input.steps.push_back (step);
    }
    input.hoursPerStep = double (record.timestepMinutes) / 60.0;
    input.radius = job.key.radius;
    const auto& snapshot = *record.snapshot;
    input.blocks.assign (snapshot.meshes.size (), 1);
    if (record.elementRoles.size () == snapshot.meshes.size ())
        for (size_t m = 0; m < snapshot.meshes.size (); ++m)
            input.blocks[m] = record.elementRoles[m] != uint8_t (evp::sunstudy::ElementRole::Ignored) ? 1 : 0;
    // Already lit: a clear ray to that sun now, through the study's occluders.
    const auto lit = [&record, &input] (const double direction[3]) {
        if (record.occluders == nullptr)
            return true;
        const double origin[3] = { input.point[0] + 0.001 * direction[0], input.point[1] + 0.001 * direction[1],
                                   input.point[2] + 0.001 * direction[2] };
        for (const auto& engine : { record.occluders->analysis, record.occluders->context })
            if (engine != nullptr && engine->Raycast (origin, direction, 0.0).hit)
                return false;
        return true;
    };
    frame->attributedSteps = input.steps.size ();
    if (!input.steps.empty ()) {
        frame->firstMinute = input.steps.front ().time.hour * 60 + input.steps.front ().time.minute;
        frame->lastMinute = input.steps.back ().time.hour * 60 + input.steps.back ().time.minute;
    }
    // Stage one: the cutting surface, as far as the farthest blocker the
    // slice's rays meet now (the full cut later trims it to what it cuts).
    {
        double reach = 0.0;
        if (record.occluders != nullptr)
            for (const auto& step : input.steps) {
                double direction[3] = { step.direction[0], step.direction[1], step.direction[2] };
                const double length =
                    std::sqrt (direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
                if (!(length > 0.0) || direction[2] <= 0.0)
                    continue;
                for (double& c : direction)
                    c /= length;
                for (const auto& engine : { record.occluders->analysis, record.occluders->context })
                    if (engine != nullptr) {
                        const auto hit = engine->Raycast (input.point, direction, 0.0);
                        if (hit.hit)
                            reach = (std::max) (reach, hit.t);
                    }
            }
        evp::sunstudy::SolarCutResult surface;
        if (evp::sunstudy::ComputeSolarCutSurface (input, surface, lit, reach > 0.0 ? reach * 1.05 + 1.0 : 20.0) &&
            !surface.envelope.empty () && publish) {
            if (cancelled.load ())
                return nullptr;
            frame->facingHours = surface.facingHours;
            frame->litHours = surface.litHours;
            frame->sectors = surface.sectors;
            frame->cutStage = SunSolarCutStage::Surface;
            BuildSolarCutGeometry (surface, input.point, frame->drawables);
            publish (*frame);
            frame->drawables = GhPreviewDrawables ();
        }
    }
    evp::sunstudy::SolarCutResult cut;
    if (!evp::sunstudy::ComputeSolarCut (snapshot, input, cut, lit, [&] { return cancelled.load (); })) {
        if (cancelled.load ())
            return nullptr;
        frame->error = cut.error;
        frame->cutStage = SunSolarCutStage::Done;
        return frame;
    }
    frame->removedVolume = cut.removedVolume;
    frame->facingHours = cut.facingHours;
    frame->litHours = cut.litHours;
    frame->sectors = cut.sectors;
    std::set<std::string> cutGuids;
    for (const auto& object : cut.objects) {
        frame->cutObjects.push_back ({ object.guid, object.elemType, object.removed, object.total, object.closed });
        frame->blockerGuids.push_back (object.guid);
        cutGuids.insert (evp::sunstudy::CanonicalGuid (object.guid));
    }
    frame->blockerObjects = cut.objects.size ();
    BuildSolarCutGeometry (cut, input.point, frame->drawables);
    frame->cutStage = SunSolarCutStage::Done;
    if (cut.kept.empty ())
        return frame;
    // Stage two: the cut, while its shadows are re-traced.
    if (publish) {
        frame->cutStage = SunSolarCutStage::Cut;
        publish (*frame);
        frame->cutStage = SunSolarCutStage::Done;
    }

    // The shadows the cut removes: the scene again with the cut elements as they
    // would remain, and every study cell whose ray toward a slice sun stopped on
    // a cut element and now runs clear.
    geomsrv::Snapshot remains = snapshot;
    for (const auto& [mesh, kept] : cut.kept)
        if (mesh < remains.meshes.size ())
            remains.meshes[mesh] = kept;
    const auto roles = evp::sunstudy::ResolveElementRoles (remains, record.analysisElements, record.contextElements,
                                                           record.ignoredElements);
    const auto after =
        evp::sunstudy::BuildSunStudyOccluders (remains, roles, nullptr, [&] { return cancelled.load (); });
    if (cancelled.load ())
        return nullptr;
    if (after == nullptr || record.occluders == nullptr)
        return frame;
    const auto clear = [&after] (const double origin[3], const double direction[3]) {
        for (const auto& engine : { after->analysis, after->context })
            if (engine != nullptr && engine->Raycast (origin, direction, 0.0).hit)
                return false;
        return true;
    };
    // The point itself, measured rather than assumed.
    for (const auto& sun : cut.facingSteps) {
        const double origin[3] = { input.point[0] + 0.001 * sun.direction[0], input.point[1] + 0.001 * sun.direction[1],
                                   input.point[2] + 0.001 * sun.direction[2] };
        if (clear (origin, sun.direction))
            frame->litAfterHours += input.hoursPerStep;
    }
    const size_t samples = record.positions.size () / 3;
    for (size_t s = 0; s < samples; ++s) {
        if ((s & 255) == 0 && cancelled.load ())
            return nullptr;
        const double* position = &record.positions[3 * s];
        const double* normal = record.normals.size () == record.positions.size () ? &record.normals[3 * s] : nullptr;
        for (const auto& step : input.steps) {
            double direction[3] = { step.direction[0], step.direction[1], step.direction[2] };
            const double length =
                std::sqrt (direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
            if (!(length > 0.0) || direction[2] <= 0.0)
                continue;
            for (double& c : direction)
                c /= length;
            if (!evp::sunstudy::FacesSun (normal, direction))
                continue;
            const double origin[3] = { position[0] + 0.001 * direction[0], position[1] + 0.001 * direction[1],
                                       position[2] + 0.001 * direction[2] };
            // Blocked now, and first by an element the cut changes?
            double nearest = -1.0;
            std::string by;
            for (const auto& engine : { record.occluders->analysis, record.occluders->context }) {
                if (engine == nullptr)
                    continue;
                const auto hit = engine->Raycast (origin, direction, 0.0);
                if (hit.hit && (nearest < 0.0 || hit.t < nearest)) {
                    nearest = hit.t;
                    by = engine->SourceSnapshot ()->meshes[hit.meshIndex].guid;
                }
            }
            if (nearest < 0.0 || cutGuids.count (evp::sunstudy::CanonicalGuid (by)) == 0 || !clear (origin, direction))
                continue;
            ++frame->wonCells; // measured-cell count only; never drives the spatial preview boundary
            break;
        }
    }
    std::vector<uint32_t> receivers;
    if (record.IsPatchDomain ()) {
        const std::set<uint32_t> measured (record.patchGrid.spanOf.begin (), record.patchGrid.spanOf.end ());
        for (size_t face = 0; face < record.patchGrid.patchOfTriangle.size (); ++face)
            if (measured.count (record.patchGrid.patchOfTriangle[face]) != 0)
                receivers.push_back (uint32_t (face));
    }
    else
        receivers = record.sampleGrid.faces;
    if (receivers.empty ())
        frame->exposure.error = "Won-lit surfaces need retained triangle/patch receiver geometry";
    else if (evp::sunstudy::ComputeSolarExposure (snapshot, cut, input.blocks, receivers, input.steps, frame->exposure,
                                                  [&] { return cancelled.load (); }))
        AppendSolarExposure (frame->exposure, frame->drawables);
    if (cancelled.load ())
        return nullptr;
    return frame;
}

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
            if (job->key.mode == SunObstructionMode::SolarCut) {
                const auto publish = [this, job, &cancelled] (const SunObstructionFrame& stage) {
                    auto shown = std::make_shared<SunObstructionFrame> (stage);
                    std::lock_guard<std::mutex> lock (mutex);
                    if (!cancelled.load () && current == job->key && generation == job->generation)
                        latest = std::move (shown);
                };
                auto frame = SolarCutFrame (*job, cancelled, publish);
                std::lock_guard<std::mutex> lock (mutex);
                if (frame != nullptr && !cancelled.load () && current == job->key && generation == job->generation)
                    latest = std::move (frame);
                return;
            }
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
    RequestKey key {
        request.studyId,
        record->storeRevision,
        record->snapshotId != 0 ? record->snapshotId : record->snapshot->id,
        record->snapshot->captureStamp,
        request.samples,
        request.mode,
        request.mode == SunObstructionMode::TimeStep ? request.step : -1,
        request.mode != SunObstructionMode::TimeStep ? request.fromMinute : 0,
        request.mode != SunObstructionMode::TimeStep ? request.toMinute : 1440,
        request.mode == SunObstructionMode::SolarCut && request.lifted,
        { request.liftedPoint[0], request.liftedPoint[1], request.liftedPoint[2] },
        request.mode == SunObstructionMode::SolarCut ? (std::max) (0.0, request.radius) : 0.0,
    };
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
