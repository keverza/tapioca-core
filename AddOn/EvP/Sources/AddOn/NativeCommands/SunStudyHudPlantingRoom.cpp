// The Planting Scheme's room measure (#278 Stage 2): RequestPlantingRoom and
// CurrentPlantingRoom, declared with the runner's other plain-value calls in
// SunStudyHudRunner.hpp. One worker thread measures a study's crown clearance
// (SunStudyCrownClearance) against the completed record's own occluders, in
// ReadPlantingSite's sample order. Touches no DevKit.

#include "NativeCommands/SunStudyHudPeriod.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"
#include "SunStudy/SunStudyCrownClearance.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "SunStudy/SunStudyThreadPolicy.hpp"

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>

namespace geomsrv::sunstudyhud {

namespace {

struct Want {
    std::string studyId;
    double layerHeight = 0.0;
};

std::mutex gRoomMutex; // guards everything below
PlantingRoom gRoom;
std::optional<Want> gWant;
std::thread gRoomWorker;
bool gRoomWorkerRunning = false;
std::atomic<bool> gRoomCancel { false };
std::atomic<size_t> gRoomDone { 0 };

void Measure (const Want& want)
{
    using evp::sunstudy::SunStudyStore;
    auto fail = [&] (const std::string& error) {
        std::lock_guard<std::mutex> lock (gRoomMutex);
        if (gRoom.studyId == want.studyId) {
            gRoom.state = PlantingRoom::State::Failed;
            gRoom.error = error;
        }
    };
    const auto record = SunStudyStore::Get ().CompletedRecord (want.studyId);
    if (record == nullptr || record->occluders == nullptr) {
        fail ("the layer's study is not complete or has no geometry");
        return;
    }
    PlantingSite site;
    std::string error;
    if (!ReadPlantingSite (want.studyId, want.layerHeight, site, error)) {
        fail (error);
        return;
    }
    {
        std::lock_guard<std::mutex> lock (gRoomMutex);
        if (gRoom.studyId == want.studyId)
            gRoom.total = site.x.size ();
    }
    auto clearance = std::make_shared<evp::sunstudy::CrownClearance> ();
    const evp::sunstudy::CrownClearanceOptions options;
    if (!evp::sunstudy::MeasureCrownClearance (
            *record->occluders, site.x, site.y, site.groundZ, site.groups, site.meshGuids, options, *clearance, error,
            [] { return gRoomCancel.load (); }, &gRoomDone)) {
        if (!gRoomCancel.load ())
            fail (error);
        return;
    }
    std::lock_guard<std::mutex> lock (gRoomMutex);
    if (gRoom.studyId == want.studyId && !gRoomCancel.load ()) {
        gRoom.state = PlantingRoom::State::Ready;
        gRoom.clearance = std::move (clearance);
    }
}

void RoomWorker ()
{
    (void) evp::sunstudy::UseInteractiveStudyPriority ();
    for (;;) {
        Want want;
        {
            std::lock_guard<std::mutex> lock (gRoomMutex);
            if (!gWant) {
                gRoomWorkerRunning = false;
                return;
            }
            want = std::move (*gWant);
            gWant.reset ();
            gRoomCancel = false;
            gRoomDone = 0;
        }
        try {
            Measure (want);
        }
        catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock (gRoomMutex);
            gRoom.state = PlantingRoom::State::Failed;
            gRoom.error = e.what ();
        }
    }
}

} // namespace

void RequestPlantingRoom (const std::string& studyId, double layerHeight)
{
    if (studyId.empty ())
        return;
    std::lock_guard<std::mutex> lock (gRoomMutex);
    if (gRoom.studyId == studyId && gRoom.state != PlantingRoom::State::Idle)
        return; // measuring, measured, or failed for good: asked once per study
    gRoom = PlantingRoom ();
    gRoom.studyId = studyId;
    gRoom.state = PlantingRoom::State::Measuring;
    gWant = Want { studyId, layerHeight };
    gRoomCancel = true; // a measure of another study stops; the worker then takes this one
    if (gRoomWorkerRunning)
        return;
    if (gRoomWorker.joinable ())
        gRoomWorker.join (); // finished: its last act was to clear the flag
    gRoomWorkerRunning = true;
    gRoomWorker = std::thread (RoomWorker);
}

PlantingRoom CurrentPlantingRoom ()
{
    std::lock_guard<std::mutex> lock (gRoomMutex);
    PlantingRoom room = gRoom;
    if (room.state == PlantingRoom::State::Measuring)
        room.done = gRoomDone.load ();
    return room;
}

namespace plantingroom {

void Shutdown ()
{
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock (gRoomMutex);
        gWant.reset ();
        gRoomCancel = true;
        worker = std::move (gRoomWorker);
    }
    if (worker.joinable ())
        worker.join ();
    std::lock_guard<std::mutex> lock (gRoomMutex);
    gRoom = PlantingRoom ();
}

} // namespace plantingroom

} // namespace geomsrv::sunstudyhud
