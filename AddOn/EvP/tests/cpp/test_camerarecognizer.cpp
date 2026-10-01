// ArchViz/Dxgi/CameraRecognizer: a camera kept across a view change (`Keep`, `Resume`)
// is taken back by a rebind on the first model frame that matches it, and dropped when
// none does -- nothing about matching is relaxed for it. The draws are context states
// as the census hands them over; the injection's setters are recorded by
// InjectionCameraStub.cpp.

#include "ArchViz/Dxgi/CameraRecognizer.hpp"
#include "ArchViz/Dxgi/InjectionCamera.hpp"

#include <gtest/gtest.h>

namespace census = geomsrv::archviz::dxgi::census;
namespace contextstate = geomsrv::archviz::dxgi::contextstate;
namespace injection = geomsrv::archviz::dxgi::injection;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {
namespace stub {
extern CameraSource source;
extern uint32_t interpretation;
extern uint32_t occurrence;
} // namespace stub
} // namespace injection
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

namespace {

constexpr uint32_t kOccurrence = 2;
constexpr uint32_t kIndexCount = 216;
constexpr uint32_t kInterpretation = 8;

// The model's camera draw, as the 3D window at 3013x1854 drew it on 2026-09-30.
contextstate::ContextState CameraDraw (uint64_t session)
{
    contextstate::ContextState live;
    live.renderTarget = 0x1000 + session; // this session's objects: other addresses
    live.depthStencil = 0x2000 + session;
    live.viewportWidth = 3013.0f;
    live.viewportHeight = 1854.0f;
    live.renderTargetDesc = { true, 3014, 1854, 87, 1 };
    live.depthStencilDesc = { true, 3014, 1854, 20, 1 };
    live.vsConstantBuffers[1] = { 0x3000 + session, 64, 16 };
    live.vsConstantBuffers[2] = { 0x3000 + session, 80, 16 };
    return live;
}

census::KeptCamera Kept ()
{
    census::KeptCamera kept;
    kept.valid = true;
    kept.selection.valid = true;
    kept.selection.groupId = 3;
    kept.selection.occurrenceIndex = kOccurrence;
    kept.selection.variant = kInterpretation;
    kept.selection.samples = 93;
    kept.selection.renderTarget = 0x1000; // the old session's
    kept.fingerprint.valid = true;
    kept.fingerprint.occurrenceIndex = kOccurrence;
    kept.fingerprint.viewportWidth = 3013.0f;
    kept.fingerprint.viewportHeight = 1854.0f;
    kept.fingerprint.drawKindMask = 1u << uint32_t (census::DrawKind::Indexed);
    kept.fingerprint.indexCount = kIndexCount;
    kept.fingerprint.viewNumConstants = 16;
    kept.fingerprint.projectionNumConstants = 16;
    kept.fingerprint.depthPresent = true;
    kept.fingerprint.renderTargetWidth = 3014;
    kept.fingerprint.renderTargetHeight = 1854;
    kept.fingerprint.renderTargetFormat = 87;
    kept.fingerprint.renderTargetSamples = 1;
    kept.fingerprint.depthWidth = 3014;
    kept.fingerprint.depthHeight = 1854;
    kept.fingerprint.depthFormat = 20;
    kept.fingerprint.depthSamples = 1;
    kept.fingerprint.variant = kInterpretation;
    kept.calibrated = true;
    kept.cameraIndexCount = kIndexCount;
    return kept;
}

// What an arm leaves: no selection, no counts, the old session's revision still noted.
void Arm ()
{
    census::ClearSelection ();
    census::ShutdownRecognizer ();
    census::ResetBindingStats ();
    injection::stub::source = injection::CameraSource::None;
}

void Draw (const contextstate::ContextState& live, uint32_t indexCount, uint64_t modelGeneration)
{
    census::MaintainBinding (live, census::DrawKind::Indexed, indexCount, kOccurrence, modelGeneration);
}

} // namespace

// ⚠️ THE USER, 2026-09-30 15:09:44: 3D -> plan -> 3D relearned the camera from 32 samples
// over 96 model frames and locked 22 s later, after an orbit. Kept, it is committed at the
// arm as one transaction (§4) and Reacquiring; the first model frame that matches every
// term rebinds it -- as after a resize (§3) -- and before the first model generation
// nothing is matched, because occurrences then count across frames.
TEST (CameraRecognizer, AKeptCameraIsRemadeOnTheFirstModelFrameThatMatchesIt)
{
    census::NoteModelRevision (0);
    Arm ();
    census::Resume (Kept ());

    EXPECT_EQ (injection::stub::source, injection::CameraSource::CensusSelectedGroup);
    EXPECT_EQ (injection::stub::interpretation, kInterpretation);
    EXPECT_EQ (injection::stub::occurrence, kOccurrence);
    EXPECT_EQ (census::GetSelection ().groupId, 0u) << "a run-local id names nothing in this session";
    EXPECT_EQ (census::GetSelection ().renderTarget, 0u) << "nothing of the old session's objects";
    EXPECT_EQ (census::KeptCameraIndexCount (), kIndexCount);
    EXPECT_EQ (census::GetBindingStats ().resumes, 1u);
    EXPECT_EQ (census::GetLifecycle (true, 0), census::Lifecycle::Reacquiring);
    EXPECT_FALSE (census::WantsSelectionAttempt (0)) << "a calibrated camera is not chosen again";

    const contextstate::ContextState live = CameraDraw (7);
    Draw (live, kIndexCount, 0);
    EXPECT_EQ (census::GetBindingStats ().rebinds, 0u) << "no model generation yet";

    Draw (live, kIndexCount, 1);
    EXPECT_EQ (census::GetBindingStats ().rebinds, 1u);
    EXPECT_EQ (census::GetLifecycle (true, 1), census::Lifecycle::Locked);
    EXPECT_TRUE (census::MatchesSelection (live, kOccurrence));
    EXPECT_EQ (census::GetSelection ().renderTarget, live.renderTarget) << "this session's draw";

    census::AdoptGroup (5);
    census::AdoptGroup (9);
    EXPECT_EQ (census::GetSelection ().groupId, 5u) << "its group here is the first it was snapshotted in";

    const census::KeptCamera again = census::Keep ();
    EXPECT_TRUE (again.valid) << "kept again at the next view change";
    EXPECT_EQ (again.fingerprint.occurrenceIndex, kOccurrence);
    EXPECT_EQ (again.fingerprint.variant, kInterpretation);
    EXPECT_TRUE (again.calibrated);
}

// ⚠️ AND IT FAILS SAFE: a kept camera no draw matches -- an index count nothing reported
// changing -- is dropped after its grace of model frames, never adopted, and the census
// chooses with what it scored meanwhile.
TEST (CameraRecognizer, AKeptCameraNoDrawMatchesIsDroppedForTheCensus)
{
    census::NoteModelRevision (0);
    Arm ();
    census::Resume (Kept ());

    const contextstate::ContextState live = CameraDraw (8);
    for (uint64_t generation = 1; generation <= 5; ++generation)
        Draw (live, kIndexCount + 84, generation);
    EXPECT_EQ (census::GetLifecycle (true, 5), census::Lifecycle::Reacquiring) << "within its grace";
    EXPECT_EQ (census::GetBindingStats ().rebinds, 0u);

    Draw (live, kIndexCount + 84, 6);
    EXPECT_EQ (census::GetBindingStats ().resumesDropped, 1u);
    EXPECT_EQ (census::GetLifecycle (true, 6), census::Lifecycle::Learning);
    EXPECT_FALSE (census::GetFingerprint ().valid);
    EXPECT_EQ (injection::stub::source, injection::CameraSource::Learner);
    EXPECT_EQ (census::KeptCameraIndexCount (), 0u);
    EXPECT_TRUE (census::WantsSelectionAttempt (6));
}

// ⚠️ THE NEW SESSION'S MODEL REVISION STARTS AGAIN, AND THAT IS NOT AN EDIT. The watch
// restarts its count at every start; read against the old session's, the restart would
// let an index count be adopted that no edit reported. An edit the new session reports
// is adopted as ever.
TEST (CameraRecognizer, AKeptCameraTakesTheNewSessionsRevisionAsItsOwn)
{
    census::NoteModelRevision (5);
    Arm ();
    census::Resume (Kept ());
    census::NoteModelRevision (0);

    const contextstate::ContextState live = CameraDraw (9);
    Draw (live, kIndexCount + 12, 1);
    EXPECT_EQ (census::GetBindingStats ().modelEditRebinds, 0u);
    EXPECT_EQ (census::GetBindingStats ().rebinds, 0u);

    census::NoteModelRevision (1);
    Draw (live, kIndexCount + 12, 2);
    EXPECT_EQ (census::GetBindingStats ().modelEditRebinds, 1u);
    EXPECT_EQ (census::GetBindingStats ().rebinds, 1u);
    EXPECT_EQ (census::GetLifecycle (true, 2), census::Lifecycle::Locked);
}

TEST (CameraRecognizer, NothingSelectedIsNothingKept)
{
    Arm ();
    EXPECT_FALSE (census::Keep ().valid);
    census::Resume (census::Keep ());
    EXPECT_EQ (census::GetBindingStats ().resumes, 0u);
    EXPECT_EQ (census::GetLifecycle (true, 0), census::Lifecycle::Learning);
    EXPECT_EQ (injection::stub::source, injection::CameraSource::None);
}
