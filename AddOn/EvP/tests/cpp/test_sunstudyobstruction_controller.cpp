#include "ArchViz/SunStudyObstructionController.hpp"

#include "MeshFixtures.hpp"
#include "SunStudy/SunStudyOccluders.hpp"
#include "SunStudy/SunStudyRustStudy.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "sunstudy_core_fixture.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace geomsrv::archviz;

namespace {

struct StoreReset {
    ~StoreReset ()
    {
        evp::sunstudy::SunStudyStore::Get ().Clear ();
    }
};

std::string InsertDay (bool defaultRayBounds = true)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = 55;
    snapshot->captureStamp = 17;
    snapshot->meshes = { evptest::MakeBox ("roof", 0, 0, 5) };
    auto record = std::make_unique<evp::sunstudy::StudyRecord> ();
    record->snapshot = snapshot;
    record->snapshotId = snapshot->id;
    record->defaultRayBounds = defaultRayBounds;
    record->positions = { 0.25, 0.25, 0.0 };
    record->occluders =
        evp::sunstudy::BuildSunStudyOccluders (*snapshot, evp::sunstudy::ResolveElementRoles (*snapshot, {}, {}, {}));
    std::vector<evp::sunstudy::SunStep> steps (2);
    for (size_t index = 0; index < steps.size (); ++index) {
        steps[index].time = evp::sunstudy::TimeOfDay { 8 + int (index), 0 };
        steps[index].direction[2] = index == 0 ? 1.0 : -1.0;
        steps[index].altitudeDegrees = 45.0;
    }
    record->series = evp::sunstudy::SunSeries::FromSteps (steps, 60);
    record->traversal = std::make_shared<evp::sunstudy::CpuTraversal> (record->occluders->analysis);
    evp::sunstudy::StudyInputs inputs;
    inputs.geometryVersion = snapshot->id;
    inputs.sunVersion = record->series.Version ();
    inputs.gridVersion = 1;
    evp::sunstudy::fixture::AttachTestCore (*record, inputs);
    auto& store = evp::sunstudy::SunStudyStore::Get ();
    const std::string id = store.Insert (std::move (record));
    size_t advanced = 0;
    std::string error;
    if (!store.Advance (id, 2, 1, 0.001, 0.0, advanced, error))
        return "";
    return id;
}

std::shared_ptr<const SunObstructionFrame> WaitForFrame (SunObstructionController& controller,
                                                         const SunObstructionRequest& request)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (std::chrono::steady_clock::now () < deadline) {
        controller.Request (request);
        if (const auto frame = controller.Latest (); frame != nullptr)
            return frame;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    return nullptr;
}

void WaitForIdle (SunObstructionController& controller, const SunObstructionRequest& request)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (controller.Busy () && std::chrono::steady_clock::now () < deadline) {
        controller.Request (request);
        (void) controller.Latest (); // polls completion and releases the active ticket
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
}

SunObstructionRequest Request (const std::string& id)
{
    SunObstructionRequest request;
    request.studyId = id;
    request.samples = { 0 };
    request.mode = SunObstructionMode::TimeStep;
    request.step = 0;
    return request;
}

TEST (SunStudyObstructionController, SingleDayStepAndWholeDayPublishOnlyTheLatestRequest)
{
    StoreReset reset;
    const std::string id = InsertDay ();
    ASSERT_FALSE (id.empty ());
    SunObstructionController controller;
    auto request = Request (id);
    auto frame = WaitForFrame (controller, request);
    ASSERT_NE (frame, nullptr);
    EXPECT_TRUE (frame->error.empty ()) << frame->error;
    EXPECT_EQ (frame->minute, 480);
    EXPECT_EQ (frame->captureStamp, 17u);
    EXPECT_EQ (frame->blockerObjects, 1u);
    ASSERT_EQ (frame->blockers.size (), 1u);
    EXPECT_EQ (frame->blockerGuids, (std::vector<std::string> { "roof" }));
    EXPECT_EQ (frame->blockers[0].intervals, (std::vector<std::pair<int, int>> { { 480, 540 } }));
    EXPECT_GT (frame->footprints, 0u); // the cell's footprint, not the roof's whole triangle
    EXPECT_EQ (frame->blocked, 1u);

    // A clock change retires the old frame immediately and owns the next result.
    request.step = 1;
    controller.Request (request);
    if (const auto intermediate = controller.Latest (); intermediate != nullptr)
        EXPECT_EQ (intermediate->minute, 540); // a fast new result is fine; the old step is not
    frame = WaitForFrame (controller, request);
    ASSERT_NE (frame, nullptr);
    EXPECT_EQ (frame->minute, 540);
    EXPECT_EQ (frame->blockerObjects, 0u);
    EXPECT_EQ (frame->footprints, 0u);
    EXPECT_EQ (frame->lit, 1u);

    request.mode = SunObstructionMode::WholeDay;
    request.step = -1;
    frame = WaitForFrame (controller, request);
    ASSERT_NE (frame, nullptr);
    EXPECT_EQ (frame->mode, SunObstructionMode::WholeDay);
    EXPECT_EQ (frame->minute, -1);
    EXPECT_EQ (frame->blockerObjects, 1u);
    EXPECT_EQ (frame->attributedSteps, 2u);
    EXPECT_DOUBLE_EQ (frame->blockers[0].hours, 1.0);

    // Whole day attributes only the selected slice, and says so.
    request.fromMinute = 500;
    request.toMinute = 1440;
    frame = WaitForFrame (controller, request);
    ASSERT_NE (frame, nullptr);
    EXPECT_EQ (frame->attributedSteps, 1u);
    EXPECT_EQ (frame->firstMinute, 540);
    EXPECT_EQ (frame->blockerObjects, 0u);
    controller.Request ({});
    EXPECT_EQ (controller.Latest (), nullptr);
}

TEST (SunStudyObstructionController, TimeStepRefusalPublishesOnceInsteadOfRetrying)
{
    StoreReset reset;
    const std::string id = InsertDay (false);
    ASSERT_FALSE (id.empty ());
    SunObstructionController controller;
    const auto request = Request (id);
    const auto frame = WaitForFrame (controller, request);
    ASSERT_NE (frame, nullptr);
    EXPECT_NE (frame->error.find ("custom ray bounds"), std::string::npos);
    WaitForIdle (controller, request);
    EXPECT_FALSE (controller.Busy ());
    for (int repeat = 0; repeat < 3; ++repeat) {
        controller.Request (request);
        EXPECT_EQ (controller.Latest (), frame);
    }
}

TEST (SunStudyObstructionController, FreshnessNeedsTheSharedSnapshotAndItsCaptureStamp)
{
    SunObstructionFrame frame;
    frame.snapshotId = 55;
    frame.captureStamp = 17;
    auto shared = std::make_shared<geomsrv::Snapshot> ();
    shared->id = 55;
    shared->captureStamp = 17;
    EXPECT_TRUE (SunObstructionSnapshotCurrent (frame, shared, 17));
    EXPECT_FALSE (SunObstructionSnapshotCurrent (frame, shared, 18)); // capture invalidated/in flight
    shared->captureStamp = 18;
    EXPECT_FALSE (SunObstructionSnapshotCurrent (frame, shared, 18));
    shared->id = 56;
    shared->captureStamp = 17;
    EXPECT_FALSE (SunObstructionSnapshotCurrent (frame, shared, 17));
    EXPECT_FALSE (SunObstructionSnapshotCurrent (frame, nullptr, 17));
}

} // namespace
