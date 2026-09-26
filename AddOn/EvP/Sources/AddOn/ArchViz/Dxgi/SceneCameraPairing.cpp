// ArchViz/Dxgi/SceneCameraPairing -- independent image/camera pairing.
// Bound by private/docs/architecture/diligent/OVERLAY-INVARIANTS.md: this file
// observes verified host draws and the established Present camera binding only.

#include "ArchViz/Dxgi/SceneCameraPairing.hpp"

#include "ArchViz/Dxgi/ImageTransferTrace.hpp"

#include <d3d11.h>
#include <dxgi.h>

#include <atomic>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace scenecamerapairing {

namespace {

struct ImageWitness {
    uint64_t provenanceEpoch = 0;
    uint64_t rootEventSerial = 0;
    uint64_t scenePass = 0;
    uint64_t modelGeneration = 0;
    uint64_t targetEpoch = 0;
    uint64_t drawSequence = 0;
    uint64_t sceneColorResource = 0;
    uint64_t imageGeneration = 0; // Stage 72: the generation stamp at root commit
};

struct CameraWitness {
    uint64_t provenanceEpoch = 0;
    uint64_t snapshotEventSerial = 0;
    uint64_t cameraSerial = 0;
    uint64_t sourcePass = 0;
    uint64_t imageGeneration = 0; // Stage 72: the generation current at this snapshot
};

// Stage 72 -- published by `OnHostDraw`/`OnClearRenderTarget`/`OnResourceWritten`
// at every touch of the nominated back buffer B, read by `BeginPresent` into the
// pending row. `sameGeneration` flags a handoff whose image generation repeats
// the previous handoff's, which makes the unit invalid for that Present.
struct HandoffWitness {
    uint64_t provenanceEpoch = 0;
    uint64_t handoffSerial = 0;
    uint64_t imageGeneration = 0;
    bool rooted = false;
    uint64_t rootPass = 0;
    BackBufferState state = BackBufferState::None;
    bool sameGeneration = false;
};

template<typename Witness> struct Publication;

template<> struct Publication<ImageWitness> {
    std::atomic<uint64_t> version { 0 };
    std::atomic<uint64_t> provenanceEpoch { 0 };
    std::atomic<uint64_t> rootEventSerial { 0 };
    std::atomic<uint64_t> scenePass { 0 };
    std::atomic<uint64_t> modelGeneration { 0 };
    std::atomic<uint64_t> targetEpoch { 0 };
    std::atomic<uint64_t> drawSequence { 0 };
    std::atomic<uint64_t> sceneColorResource { 0 };
    std::atomic<uint64_t> imageGeneration { 0 };
};

template<> struct Publication<CameraWitness> {
    std::atomic<uint64_t> version { 0 };
    std::atomic<uint64_t> provenanceEpoch { 0 };
    std::atomic<uint64_t> snapshotEventSerial { 0 };
    std::atomic<uint64_t> cameraSerial { 0 };
    std::atomic<uint64_t> sourcePass { 0 };
    std::atomic<uint64_t> imageGeneration { 0 };
};

template<> struct Publication<HandoffWitness> {
    std::atomic<uint64_t> version { 0 };
    std::atomic<uint64_t> provenanceEpoch { 0 };
    std::atomic<uint64_t> handoffSerial { 0 };
    std::atomic<uint64_t> imageGeneration { 0 };
    std::atomic<bool> rooted { false };
    std::atomic<uint64_t> rootPass { 0 };
    std::atomic<uint32_t> state { uint32_t (BackBufferState::None) };
    std::atomic<bool> sameGeneration { false };
};

struct RowSlot {
    std::atomic<uint64_t> published { 0 };
    std::atomic<uint64_t> provenanceEpoch { 0 };
    std::atomic<uint64_t> eventSerial { 0 };
    std::atomic<uint64_t> presentSerial { 0 };
    std::atomic<uint64_t> imagePass { 0 };
    std::atomic<uint64_t> imageRootEventSerial { 0 };
    std::atomic<uint64_t> imageSourceResource { 0 };
    std::atomic<uint64_t> imageModelGeneration { 0 };
    std::atomic<uint64_t> overlayCameraSerial { 0 };
    std::atomic<uint64_t> cameraSourcePass { 0 };
    std::atomic<uint64_t> cameraSnapshotEventSerial { 0 };
    std::atomic<uint64_t> cameraAdoptEventSerial { 0 };
    std::atomic<uint64_t> ambiguityEventSerial { 0 };
    std::atomic<uint64_t> cameraMetadataPass { 0 };
    std::atomic<uint64_t> backBuffer { 0 };
    std::atomic<int64_t> delta { 0 };
    std::atomic<uint32_t> relation { uint32_t (Relation::Unknown) };
    std::atomic<bool> imageOnBackBuffer { false };
    std::atomic<bool> cameraCoherent { false };
    std::atomic<bool> presentContextOverlap { false };
    std::atomic<uint64_t> handoffSerial { 0 };
    std::atomic<uint64_t> imageGeneration { 0 };
    std::atomic<bool> imageRooted { false };
    std::atomic<uint64_t> cameraImageGeneration { 0 };
    std::atomic<bool> metadataStale { false };
    std::atomic<uint32_t> backBufferState { uint32_t (BackBufferState::None) };
};

std::atomic<bool> g_enabled { false };
std::atomic<uint64_t> g_provenanceEpoch { 0 };
std::atomic<uint64_t> g_eventSerial { 0 };
std::atomic<uint64_t> g_contextOperationVersion { 0 };
std::atomic<uint32_t> g_contextOperations { 0 };
thread_local bool g_contextOperationActive = false;
Publication<ImageWitness> g_image;
Publication<CameraWitness> g_camera;
Publication<HandoffWitness> g_handoff;
ImageWitness g_pendingImage;
bool g_pendingImageValid = false;
// Stage 72 -- context-thread-owned (written only inside the
// Enabled()+g_contextOperationActive gate; reset from the main thread only
// after the shared PassProvenance drain, like `g_pendingImage` above).
uint64_t g_sceneColour = 0;
bool g_imageRooted = false;
uint64_t g_imageRootPass = 0;
uint32_t g_rootsInGeneration = 0;
uint64_t g_rootCommits = 0;
HandoffWitness g_lastHandoff; // shadow of the last published handoff, so a
                              // state-only amend (Mixed/Cleared) keeps the rest
std::atomic<uint64_t> g_imageGeneration { 0 }; // context-thread writes; GetStats reads cross-thread
std::atomic<uint64_t> g_backBuffer { 0 };       // published by the Present thread, read by the context thread
// A resize invalidates whatever handoff B carried. The resize runs on the
// Present side, and the handoff publication and `g_lastHandoff` belong to the
// context thread, so the resize does not touch them: it records an event serial
// here and `BeginPresent` treats any handoff older than it as None.
std::atomic<uint64_t> g_resizeFenceSerial { 0 };
Row g_pendingPresent;
uint64_t g_pendingContextOperationVersion = 0;
bool g_pendingContextOverlap = false;
bool g_pendingPresentActive = false;
uint64_t g_pendingCurrentModelGeneration = 0;
bool g_pendingHandoffSameGeneration = false;
// Stage 72 -- uniqueness now keys on `handoffSerial`, not `imagePass`:
// `imageOnBackBuffer`/`imagePass` are reported fields only (see Row).
uint64_t g_lastUniqueHandoffSerial = 0;
RowSlot g_rows[kRowCapacity];
std::atomic<uint64_t> g_rowsWritten { 0 };
std::atomic<uint64_t> g_rowsVisibleFrom { 0 };
std::atomic<uint64_t> g_imageDrawsCommitted { 0 };
std::atomic<uint64_t> g_cameraSnapshots { 0 };
std::atomic<uint64_t> g_cameraBindings { 0 };
std::atomic<uint64_t> g_presents { 0 };
std::atomic<uint64_t> g_matched { 0 };
std::atomic<uint64_t> g_mismatched { 0 };
std::atomic<uint64_t> g_unknown { 0 };
std::atomic<uint64_t> g_ambiguous { 0 };
std::atomic<uint64_t> g_uniqueImagePassesObserved { 0 };
std::atomic<uint64_t> g_uniqueImagePassesClassified { 0 };
std::atomic<uint64_t> g_uniqueMatched { 0 };
std::atomic<uint64_t> g_uniqueMismatched { 0 };
std::atomic<uint64_t> g_uniqueUnknown { 0 };
std::atomic<uint64_t> g_uniqueAmbiguous { 0 };
std::atomic<uint64_t> g_duplicatePresents { 0 };
std::atomic<uint64_t> g_handoffs { 0 };
std::atomic<uint64_t> g_handoffsUnrooted { 0 };
std::atomic<uint64_t> g_handoffsSameGeneration { 0 };
std::atomic<uint64_t> g_generationsWithMultipleRoots { 0 };
std::atomic<uint64_t> g_sceneColourChanges { 0 };
std::atomic<uint64_t> g_backBufferClears { 0 };
std::atomic<uint64_t> g_backBufferOtherWrites { 0 };
std::atomic<uint64_t> g_presentsWithoutBinding { 0 };
std::atomic<uint64_t> g_uniqueDelta[kDeltaBucketCount];
std::atomic<uint64_t> g_repeatMatched { 0 };
std::atomic<uint64_t> g_repeatMismatched { 0 };
std::atomic<uint64_t> g_repeatUnknown { 0 };
std::atomic<uint64_t> g_repeatAmbiguous { 0 };
std::atomic<uint64_t> g_repeatDelta[kDeltaBucketCount];

uint64_t NextEvent ()
{
    return g_eventSerial.fetch_add (1, std::memory_order_relaxed) + 1;
}

void ClearImagePublication ()
{
    g_image.version.fetch_add (1, std::memory_order_acq_rel);
    g_image.provenanceEpoch.store (0, std::memory_order_relaxed);
    g_image.rootEventSerial.store (0, std::memory_order_relaxed);
    g_image.scenePass.store (0, std::memory_order_relaxed);
    g_image.modelGeneration.store (0, std::memory_order_relaxed);
    g_image.targetEpoch.store (0, std::memory_order_relaxed);
    g_image.drawSequence.store (0, std::memory_order_relaxed);
    g_image.sceneColorResource.store (0, std::memory_order_relaxed);
    g_image.imageGeneration.store (0, std::memory_order_relaxed);
    g_image.version.fetch_add (1, std::memory_order_release);
}

void PublishImage (const ImageWitness& image)
{
    g_image.version.fetch_add (1, std::memory_order_acq_rel);
    g_image.provenanceEpoch.store (image.provenanceEpoch, std::memory_order_relaxed);
    g_image.rootEventSerial.store (image.rootEventSerial, std::memory_order_relaxed);
    g_image.scenePass.store (image.scenePass, std::memory_order_relaxed);
    g_image.modelGeneration.store (image.modelGeneration, std::memory_order_relaxed);
    g_image.targetEpoch.store (image.targetEpoch, std::memory_order_relaxed);
    g_image.drawSequence.store (image.drawSequence, std::memory_order_relaxed);
    g_image.sceneColorResource.store (image.sceneColorResource, std::memory_order_relaxed);
    g_image.imageGeneration.store (image.imageGeneration, std::memory_order_relaxed);
    g_image.version.fetch_add (1, std::memory_order_release);
}

bool ReadImage (ImageWitness& image)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t before = g_image.version.load (std::memory_order_acquire);
        if ((before & 1u) != 0)
            continue;
        ImageWitness candidate;
        candidate.provenanceEpoch = g_image.provenanceEpoch.load (std::memory_order_relaxed);
        candidate.rootEventSerial = g_image.rootEventSerial.load (std::memory_order_relaxed);
        candidate.scenePass = g_image.scenePass.load (std::memory_order_relaxed);
        candidate.modelGeneration = g_image.modelGeneration.load (std::memory_order_relaxed);
        candidate.targetEpoch = g_image.targetEpoch.load (std::memory_order_relaxed);
        candidate.drawSequence = g_image.drawSequence.load (std::memory_order_relaxed);
        candidate.sceneColorResource = g_image.sceneColorResource.load (std::memory_order_relaxed);
        candidate.imageGeneration = g_image.imageGeneration.load (std::memory_order_relaxed);
        const uint64_t after = g_image.version.load (std::memory_order_acquire);
        if (before == after) {
            image = candidate;
            return candidate.provenanceEpoch != 0 && candidate.scenePass != 0;
        }
    }
    return false;
}

void PublishCamera (const CameraWitness& camera)
{
    g_camera.version.fetch_add (1, std::memory_order_acq_rel);
    g_camera.provenanceEpoch.store (camera.provenanceEpoch, std::memory_order_relaxed);
    g_camera.snapshotEventSerial.store (camera.snapshotEventSerial, std::memory_order_relaxed);
    g_camera.cameraSerial.store (camera.cameraSerial, std::memory_order_relaxed);
    g_camera.sourcePass.store (camera.sourcePass, std::memory_order_relaxed);
    g_camera.imageGeneration.store (camera.imageGeneration, std::memory_order_relaxed);
    g_camera.version.fetch_add (1, std::memory_order_release);
}

bool ReadCamera (CameraWitness& camera)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t before = g_camera.version.load (std::memory_order_acquire);
        if ((before & 1u) != 0)
            continue;
        CameraWitness candidate;
        candidate.provenanceEpoch = g_camera.provenanceEpoch.load (std::memory_order_relaxed);
        candidate.snapshotEventSerial = g_camera.snapshotEventSerial.load (std::memory_order_relaxed);
        candidate.cameraSerial = g_camera.cameraSerial.load (std::memory_order_relaxed);
        candidate.sourcePass = g_camera.sourcePass.load (std::memory_order_relaxed);
        candidate.imageGeneration = g_camera.imageGeneration.load (std::memory_order_relaxed);
        const uint64_t after = g_camera.version.load (std::memory_order_acquire);
        if (before == after) {
            camera = candidate;
            return candidate.provenanceEpoch != 0 && candidate.cameraSerial != 0 && candidate.sourcePass != 0;
        }
    }
    return false;
}

void PublishHandoff (const HandoffWitness& handoff)
{
    g_handoff.version.fetch_add (1, std::memory_order_acq_rel);
    g_handoff.provenanceEpoch.store (handoff.provenanceEpoch, std::memory_order_relaxed);
    g_handoff.handoffSerial.store (handoff.handoffSerial, std::memory_order_relaxed);
    g_handoff.imageGeneration.store (handoff.imageGeneration, std::memory_order_relaxed);
    g_handoff.rooted.store (handoff.rooted, std::memory_order_relaxed);
    g_handoff.rootPass.store (handoff.rootPass, std::memory_order_relaxed);
    g_handoff.state.store (uint32_t (handoff.state), std::memory_order_relaxed);
    g_handoff.sameGeneration.store (handoff.sameGeneration, std::memory_order_relaxed);
    g_handoff.version.fetch_add (1, std::memory_order_release);
}

// CONTEXT THREAD ONLY. Amends `g_lastHandoff`'s state (Handoff -> Mixed on an
// untracked write, or -> Cleared on a clear of B) and republishes it whole, so
// the other fields a Present might still read (handoffSerial, imageGeneration,
// rooted, rootPass, sameGeneration) survive a state-only transition.
void PublishHandoffState (BackBufferState state)
{
    g_lastHandoff.provenanceEpoch = g_provenanceEpoch.load (std::memory_order_relaxed);
    g_lastHandoff.state = state;
    PublishHandoff (g_lastHandoff);
}

bool ReadHandoff (HandoffWitness& handoff)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t before = g_handoff.version.load (std::memory_order_acquire);
        if ((before & 1u) != 0)
            continue;
        HandoffWitness candidate;
        candidate.provenanceEpoch = g_handoff.provenanceEpoch.load (std::memory_order_relaxed);
        candidate.handoffSerial = g_handoff.handoffSerial.load (std::memory_order_relaxed);
        candidate.imageGeneration = g_handoff.imageGeneration.load (std::memory_order_relaxed);
        candidate.rooted = g_handoff.rooted.load (std::memory_order_relaxed);
        candidate.rootPass = g_handoff.rootPass.load (std::memory_order_relaxed);
        candidate.state = BackBufferState (g_handoff.state.load (std::memory_order_relaxed));
        candidate.sameGeneration = g_handoff.sameGeneration.load (std::memory_order_relaxed);
        const uint64_t after = g_handoff.version.load (std::memory_order_acquire);
        if (before == after) {
            handoff = candidate;
            return candidate.provenanceEpoch != 0;
        }
    }
    return false;
}

void ClearHandoffPublication ()
{
    g_handoff.version.fetch_add (1, std::memory_order_acq_rel);
    g_handoff.provenanceEpoch.store (0, std::memory_order_relaxed);
    g_handoff.handoffSerial.store (0, std::memory_order_relaxed);
    g_handoff.imageGeneration.store (0, std::memory_order_relaxed);
    g_handoff.rooted.store (false, std::memory_order_relaxed);
    g_handoff.rootPass.store (0, std::memory_order_relaxed);
    g_handoff.state.store (uint32_t (BackBufferState::None), std::memory_order_relaxed);
    g_handoff.sameGeneration.store (false, std::memory_order_relaxed);
    g_handoff.version.fetch_add (1, std::memory_order_release);
}

// {<=-3,-2,-1,0,+1,+2,>=+3}; index 3 (delta == 0) is MATCH, the lower three are
// the overlay camera BEHIND its image, the upper three AHEAD. Only MATCH and
// MISMATCH Presents are bucketed.
void BucketDelta (std::atomic<uint64_t> (&buckets)[kDeltaBucketCount], int64_t delta)
{
    // Clamp to [-3, 3] as a SIGNED value before shifting into an index -- casting
    // a negative delta straight to size_t would wrap to a huge unsigned value.
    const int64_t clamped = delta < -3 ? -3 : delta > 3 ? 3 : delta;
    buckets[size_t (clamped + 3)].fetch_add (1, std::memory_order_relaxed);
}

void CountRelation (Relation relation, std::atomic<uint64_t>& matched, std::atomic<uint64_t>& mismatched,
                    std::atomic<uint64_t>& unknown, std::atomic<uint64_t>& ambiguous)
{
    if (relation == Relation::Match)
        matched.fetch_add (1, std::memory_order_relaxed);
    else if (relation == Relation::Mismatch)
        mismatched.fetch_add (1, std::memory_order_relaxed);
    else if (relation == Relation::Ambiguous)
        ambiguous.fetch_add (1, std::memory_order_relaxed);
    else
        unknown.fetch_add (1, std::memory_order_relaxed);
}

void PublishRow (const Row& row)
{
    const uint64_t written = g_rowsWritten.fetch_add (1, std::memory_order_acq_rel);
    RowSlot& slot = g_rows[written % kRowCapacity];
    slot.published.store (0, std::memory_order_release);
    slot.provenanceEpoch.store (row.provenanceEpoch, std::memory_order_relaxed);
    slot.eventSerial.store (row.eventSerial, std::memory_order_relaxed);
    slot.presentSerial.store (row.presentSerial, std::memory_order_relaxed);
    slot.imagePass.store (row.imagePass, std::memory_order_relaxed);
    slot.imageRootEventSerial.store (row.imageRootEventSerial, std::memory_order_relaxed);
    slot.imageSourceResource.store (row.imageSourceResource, std::memory_order_relaxed);
    slot.imageModelGeneration.store (row.imageModelGeneration, std::memory_order_relaxed);
    slot.overlayCameraSerial.store (row.overlayCameraSerial, std::memory_order_relaxed);
    slot.cameraSourcePass.store (row.cameraSourcePass, std::memory_order_relaxed);
    slot.cameraSnapshotEventSerial.store (row.cameraSnapshotEventSerial, std::memory_order_relaxed);
    slot.cameraAdoptEventSerial.store (row.cameraAdoptEventSerial, std::memory_order_relaxed);
    slot.ambiguityEventSerial.store (row.ambiguityEventSerial, std::memory_order_relaxed);
    slot.cameraMetadataPass.store (row.cameraMetadataPass, std::memory_order_relaxed);
    slot.backBuffer.store (row.backBuffer, std::memory_order_relaxed);
    slot.delta.store (row.delta, std::memory_order_relaxed);
    slot.relation.store (uint32_t (row.relation), std::memory_order_relaxed);
    slot.imageOnBackBuffer.store (row.imageOnBackBuffer, std::memory_order_relaxed);
    slot.cameraCoherent.store (row.cameraCoherent, std::memory_order_relaxed);
    slot.presentContextOverlap.store (row.presentContextOverlap, std::memory_order_relaxed);
    slot.handoffSerial.store (row.handoffSerial, std::memory_order_relaxed);
    slot.imageGeneration.store (row.imageGeneration, std::memory_order_relaxed);
    slot.imageRooted.store (row.imageRooted, std::memory_order_relaxed);
    slot.cameraImageGeneration.store (row.cameraImageGeneration, std::memory_order_relaxed);
    slot.metadataStale.store (row.metadataStale, std::memory_order_relaxed);
    slot.backBufferState.store (uint32_t (row.backBufferState), std::memory_order_relaxed);
    slot.published.store (written + 1, std::memory_order_release);
}

} // namespace

void SetEnabled (bool enabled)
{
    g_enabled.store (enabled, std::memory_order_release);
}

bool Enabled ()
{
    return g_enabled.load (std::memory_order_acquire);
}

void Reset ()
{
    g_provenanceEpoch.fetch_add (1, std::memory_order_acq_rel);
    g_eventSerial.store (0, std::memory_order_relaxed);
    g_contextOperationVersion.store (0, std::memory_order_relaxed);
    g_contextOperations.store (0, std::memory_order_relaxed);
    g_pendingImage = ImageWitness {};
    g_pendingImageValid = false;
    g_sceneColour = 0;
    g_imageRooted = false;
    g_imageRootPass = 0;
    g_rootsInGeneration = 0;
    g_rootCommits = 0;
    g_lastHandoff = HandoffWitness {};
    g_imageGeneration.store (0, std::memory_order_relaxed);
    g_resizeFenceSerial.store (0, std::memory_order_relaxed);
    g_pendingPresent = Row {};
    g_pendingPresentActive = false;
    g_pendingCurrentModelGeneration = 0;
    g_pendingHandoffSameGeneration = false;
    g_lastUniqueHandoffSerial = 0;
    ClearImagePublication ();
    g_camera.version.fetch_add (1, std::memory_order_acq_rel);
    g_camera.provenanceEpoch.store (0, std::memory_order_relaxed);
    g_camera.snapshotEventSerial.store (0, std::memory_order_relaxed);
    g_camera.cameraSerial.store (0, std::memory_order_relaxed);
    g_camera.sourcePass.store (0, std::memory_order_relaxed);
    g_camera.imageGeneration.store (0, std::memory_order_relaxed);
    g_camera.version.fetch_add (1, std::memory_order_release);
    ClearHandoffPublication ();
    g_rowsVisibleFrom.store (g_rowsWritten.load (std::memory_order_acquire), std::memory_order_release);
    g_imageDrawsCommitted.store (0, std::memory_order_relaxed);
    g_cameraSnapshots.store (0, std::memory_order_relaxed);
    g_cameraBindings.store (0, std::memory_order_relaxed);
    g_presents.store (0, std::memory_order_relaxed);
    g_matched.store (0, std::memory_order_relaxed);
    g_mismatched.store (0, std::memory_order_relaxed);
    g_unknown.store (0, std::memory_order_relaxed);
    g_ambiguous.store (0, std::memory_order_relaxed);
    g_uniqueImagePassesObserved.store (0, std::memory_order_relaxed);
    g_uniqueImagePassesClassified.store (0, std::memory_order_relaxed);
    g_uniqueMatched.store (0, std::memory_order_relaxed);
    g_uniqueMismatched.store (0, std::memory_order_relaxed);
    g_uniqueUnknown.store (0, std::memory_order_relaxed);
    g_uniqueAmbiguous.store (0, std::memory_order_relaxed);
    g_duplicatePresents.store (0, std::memory_order_relaxed);
    g_handoffs.store (0, std::memory_order_relaxed);
    g_handoffsUnrooted.store (0, std::memory_order_relaxed);
    g_handoffsSameGeneration.store (0, std::memory_order_relaxed);
    g_generationsWithMultipleRoots.store (0, std::memory_order_relaxed);
    g_sceneColourChanges.store (0, std::memory_order_relaxed);
    g_backBufferClears.store (0, std::memory_order_relaxed);
    g_backBufferOtherWrites.store (0, std::memory_order_relaxed);
    g_presentsWithoutBinding.store (0, std::memory_order_relaxed);
    g_repeatMatched.store (0, std::memory_order_relaxed);
    g_repeatMismatched.store (0, std::memory_order_relaxed);
    g_repeatUnknown.store (0, std::memory_order_relaxed);
    g_repeatAmbiguous.store (0, std::memory_order_relaxed);
    for (std::atomic<uint64_t>& bucket : g_uniqueDelta)
        bucket.store (0, std::memory_order_relaxed);
    for (std::atomic<uint64_t>& bucket : g_repeatDelta)
        bucket.store (0, std::memory_order_relaxed);
}

bool BeginContextOperation ()
{
    if (!Enabled () || g_contextOperationActive)
        return false;
    g_contextOperations.fetch_add (1, std::memory_order_acq_rel);
    g_contextOperationVersion.fetch_add (1, std::memory_order_release);
    g_contextOperationActive = true;
    return true;
}

void EndContextOperation ()
{
    g_contextOperationActive = false;
    g_contextOperationVersion.fetch_add (1, std::memory_order_release);
    g_contextOperations.fetch_sub (1, std::memory_order_release);
}

void ArmVerifiedModelDraw (uint64_t scenePass, uint64_t modelGeneration, uint64_t targetEpoch,
                           uint64_t drawSequence, uint64_t sceneColorResource)
{
    if (!Enabled () || !g_contextOperationActive)
        return;
    g_pendingImage.provenanceEpoch = g_provenanceEpoch.load (std::memory_order_relaxed);
    g_pendingImage.scenePass = scenePass;
    g_pendingImage.modelGeneration = modelGeneration;
    g_pendingImage.targetEpoch = targetEpoch;
    g_pendingImage.drawSequence = drawSequence;
    g_pendingImage.sceneColorResource = sceneColorResource;
    g_pendingImageValid = scenePass != 0 && sceneColorResource != 0;
}

void OnDrawCompleted ()
{
    if (!g_contextOperationActive || !g_pendingImageValid)
        return;
    if (Enabled ()) {
        // Stage 72 -- this is the root commit: stamp S's identity and the image
        // generation onto the witness before publishing it. `g_pendingImageValid`
        // guarantees `sceneColorResource` is non-zero here.
        const uint64_t sceneColour = g_pendingImage.sceneColorResource;
        if (g_sceneColour != 0 && g_sceneColour != sceneColour) {
            // S was resized or recreated: the old generation count tracked
            // clears of a resource that is no longer S, so it starts over
            // instead of continuing to increment across the discontinuity.
            g_sceneColourChanges.fetch_add (1, std::memory_order_relaxed);
            g_imageGeneration.store (0, std::memory_order_relaxed);
            g_imageRooted = false;
            g_imageRootPass = 0;
            g_rootsInGeneration = 0;
        }
        g_sceneColour = sceneColour;
        g_imageRooted = true;
        g_imageRootPass = g_pendingImage.scenePass;
        if (++g_rootsInGeneration == 2)
            g_generationsWithMultipleRoots.fetch_add (1, std::memory_order_relaxed);
        g_pendingImage.imageGeneration = g_imageGeneration.load (std::memory_order_relaxed);
        ++g_rootCommits;

        g_pendingImage.rootEventSerial = NextEvent ();
        PublishImage (g_pendingImage);
        g_imageDrawsCommitted.fetch_add (1, std::memory_order_relaxed);
        imagetransfer::OnImageCommitted (g_pendingImage.scenePass, g_pendingImage.modelGeneration,
                                         g_pendingImage.sceneColorResource, g_pendingImage.rootEventSerial);
    }
    g_pendingImage = ImageWitness {};
    g_pendingImageValid = false;
}

void OnCameraSnapshot (uint64_t cameraSerial, uint64_t sourcePass)
{
    if (!Enabled () || !g_contextOperationActive || cameraSerial == 0 || sourcePass == 0)
        return;
    CameraWitness camera;
    camera.provenanceEpoch = g_provenanceEpoch.load (std::memory_order_relaxed);
    camera.snapshotEventSerial = NextEvent ();
    camera.cameraSerial = cameraSerial;
    camera.sourcePass = sourcePass;
    camera.imageGeneration = g_imageGeneration.load (std::memory_order_relaxed);
    PublishCamera (camera);
    g_cameraSnapshots.fetch_add (1, std::memory_order_relaxed);
}

// Stage 72 -- called on EVERY draw (forwarded from PassProvenance.cpp's own
// per-draw handler), because the composite draw that writes B is a separate,
// later draw from the verified model draw that renders into S: `rtvs`/`srvs`
// are the SAME per-slot arrays that handler already tracks; unbound slots read
// 0. B (`g_backBuffer`) is published by the Present thread.
void OnHostDraw (const uint64_t* rtvs, size_t rtvSlots, const uint64_t* srvs, size_t srvSlots)
{
    if (!Enabled () || !g_contextOperationActive)
        return;
    const uint64_t backBuffer = g_backBuffer.load (std::memory_order_acquire);
    if (backBuffer == 0)
        return;
    bool targetsBackBuffer = false;
    for (size_t slot = 0; slot < rtvSlots; ++slot) {
        if (rtvs[slot] == backBuffer) {
            targetsBackBuffer = true;
            break;
        }
    }
    if (!targetsBackBuffer)
        return;

    bool readsSceneColour = false;
    const uint64_t sceneColour = g_sceneColour;
    if (sceneColour != 0) {
        for (size_t slot = 0; slot < srvSlots; ++slot) {
            if (srvs[slot] == sceneColour) {
                readsSceneColour = true;
                break;
            }
        }
    }

    if (readsSceneColour) {
        HandoffWitness handoff;
        handoff.provenanceEpoch = g_provenanceEpoch.load (std::memory_order_relaxed);
        handoff.handoffSerial = NextEvent ();
        handoff.imageGeneration = g_imageGeneration.load (std::memory_order_relaxed);
        handoff.rooted = g_imageRooted;
        handoff.rootPass = g_imageRootPass;
        handoff.state = BackBufferState::Handoff;
        handoff.sameGeneration =
            g_lastHandoff.handoffSerial != 0 && handoff.imageGeneration == g_lastHandoff.imageGeneration;
        PublishHandoff (handoff);
        g_lastHandoff = handoff;
        g_handoffs.fetch_add (1, std::memory_order_relaxed);
        if (!handoff.rooted)
            g_handoffsUnrooted.fetch_add (1, std::memory_order_relaxed);
        if (handoff.sameGeneration)
            g_handoffsSameGeneration.fetch_add (1, std::memory_order_relaxed);
    }
    else {
        if (g_lastHandoff.state == BackBufferState::Handoff)
            PublishHandoffState (BackBufferState::Mixed);
        g_backBufferOtherWrites.fetch_add (1, std::memory_order_relaxed);
    }
}

void OnClearRenderTarget (uint64_t resource)
{
    if (!Enabled () || !g_contextOperationActive || resource == 0)
        return;
    if (resource == g_sceneColour) {
        g_imageGeneration.fetch_add (1, std::memory_order_relaxed);
        g_imageRooted = false;
        g_imageRootPass = 0;
        g_rootsInGeneration = 0;
    }
    else if (resource == g_backBuffer.load (std::memory_order_acquire)) {
        PublishHandoffState (BackBufferState::Cleared);
        g_backBufferClears.fetch_add (1, std::memory_order_relaxed);
    }
}

void OnResourceWritten (uint64_t resource)
{
    if (!Enabled () || !g_contextOperationActive || resource == 0)
        return;
    if (resource != g_backBuffer.load (std::memory_order_acquire))
        return;
    if (g_lastHandoff.state == BackBufferState::Handoff)
        PublishHandoffState (BackBufferState::Mixed);
    g_backBufferOtherWrites.fetch_add (1, std::memory_order_relaxed);
}

uint64_t ContextImageGeneration ()
{
    return g_imageGeneration.load (std::memory_order_relaxed);
}

uint64_t ContextRootCommits ()
{
    return g_rootCommits;
}

bool BeginPresent (IDXGISwapChain* swapChain, uint64_t currentModelGeneration)
{
    if (!Enabled () || swapChain == nullptr || g_pendingPresentActive)
        return false;
    const uint32_t operationsBefore = g_contextOperations.load (std::memory_order_acquire);
    const uint64_t operationVersionBefore = g_contextOperationVersion.load (std::memory_order_acquire);
    ID3D11Texture2D* texture = nullptr;
    const HRESULT result = swapChain->GetBuffer (0, __uuidof (ID3D11Texture2D), (void**) &texture);
    const uint64_t backBuffer = SUCCEEDED (result) && texture != nullptr
                                    ? uint64_t (uintptr_t (static_cast<ID3D11Resource*> (texture)))
                                    : 0;
    if (texture != nullptr)
        texture->Release ();
    g_backBuffer.store (backBuffer, std::memory_order_release);

    g_pendingPresent = Row {};
    g_pendingPresent.provenanceEpoch = g_provenanceEpoch.load (std::memory_order_relaxed);
    g_pendingPresent.eventSerial = NextEvent ();
    g_pendingPresent.backBuffer = backBuffer;
    ImageWitness image;
    if (ReadImage (image) && image.provenanceEpoch == g_pendingPresent.provenanceEpoch) {
        g_pendingPresent.imagePass = image.scenePass;
        g_pendingPresent.imageRootEventSerial = image.rootEventSerial;
        g_pendingPresent.imageSourceResource = image.sceneColorResource;
        g_pendingPresent.imageModelGeneration = image.modelGeneration;
        g_pendingPresent.imageOnBackBuffer = image.sceneColorResource != 0 && image.sceneColorResource == backBuffer;
    }
    // Stage 72 -- read the published handoff into the pending row. `sameGeneration`
    // is not a Row field: it only ever gates this one Present's classification.
    HandoffWitness handoff;
    if (ReadHandoff (handoff) && handoff.provenanceEpoch == g_pendingPresent.provenanceEpoch &&
        handoff.handoffSerial > g_resizeFenceSerial.load (std::memory_order_acquire)) {
        g_pendingPresent.handoffSerial = handoff.handoffSerial;
        g_pendingPresent.imageGeneration = handoff.imageGeneration;
        g_pendingPresent.imageRooted = handoff.rooted;
        g_pendingPresent.backBufferState = handoff.state;
        g_pendingHandoffSameGeneration = handoff.sameGeneration;
    }
    else {
        g_pendingHandoffSameGeneration = false;
    }
    const uint64_t operationVersionAfter = g_contextOperationVersion.load (std::memory_order_acquire);
    const uint32_t operationsAfter = g_contextOperations.load (std::memory_order_acquire);
    g_pendingContextOperationVersion = operationVersionAfter;
    g_pendingContextOverlap = operationsBefore != 0 || operationsAfter != 0 ||
                              operationVersionBefore != operationVersionAfter;
    g_pendingCurrentModelGeneration = currentModelGeneration;
    g_pendingPresentActive = true;
    return true;
}

void OnOverlayCameraBound (uint64_t metadataPass)
{
    if (!Enabled () || !g_pendingPresentActive)
        return;
    CameraWitness camera;
    if (!ReadCamera (camera) || camera.provenanceEpoch != g_pendingPresent.provenanceEpoch)
        return;
    g_pendingPresent.overlayCameraSerial = camera.cameraSerial;
    g_pendingPresent.cameraSourcePass = camera.sourcePass;
    g_pendingPresent.cameraSnapshotEventSerial = camera.snapshotEventSerial;
    g_pendingPresent.cameraAdoptEventSerial = NextEvent ();
    g_pendingPresent.cameraMetadataPass = metadataPass;
    g_pendingPresent.cameraCoherent = metadataPass != 0 && metadataPass == camera.sourcePass;
    g_pendingPresent.cameraImageGeneration = camera.imageGeneration;
    g_pendingPresent.metadataStale = metadataPass != camera.sourcePass; // reported only (S72)
    g_cameraBindings.fetch_add (1, std::memory_order_relaxed);
}

void EndPresent (bool active, bool succeeded)
{
    if (!active || !g_pendingPresentActive)
        return;
    const bool overlap = g_pendingContextOverlap || g_contextOperations.load (std::memory_order_acquire) != 0 ||
                         g_contextOperationVersion.load (std::memory_order_acquire) !=
                             g_pendingContextOperationVersion;
    if (succeeded) {
        Row& row = g_pendingPresent;
        row.presentSerial = g_presents.fetch_add (1, std::memory_order_relaxed) + 1;
        row.presentContextOverlap = overlap;
        // Stage 72 classification order (the old `imageOnBackBuffer`,
        // `imageModelGeneration` and `cameraCoherent` checks no longer gate
        // anything; they stay on Row as reported context only):
        //   1. context/Present overlap                       -> AMBIGUOUS
        //   2. back buffer Mixed                              -> AMBIGUOUS
        //      back buffer None/Cleared                       -> UNKNOWN
        //   3. no camera bound this Present                   -> UNKNOWN
        //   4. handoff or camera generation unknown (==0),
        //      or the handoff repeats the previous generation -> UNKNOWN
        //   5. delta = cameraImageGeneration - imageGeneration -> MATCH/MISMATCH
        if (overlap) {
            row.relation = Relation::Ambiguous;
            row.ambiguityEventSerial = NextEvent ();
        }
        else if (row.backBufferState == BackBufferState::Mixed) {
            row.relation = Relation::Ambiguous;
        }
        else if (row.backBufferState == BackBufferState::None || row.backBufferState == BackBufferState::Cleared) {
            row.relation = Relation::Unknown;
        }
        else if (row.overlayCameraSerial == 0) {
            row.relation = Relation::Unknown;
            g_presentsWithoutBinding.fetch_add (1, std::memory_order_relaxed);
        }
        else if (row.imageGeneration == 0 || row.cameraImageGeneration == 0 || g_pendingHandoffSameGeneration) {
            row.relation = Relation::Unknown;
        }
        else {
            row.delta = int64_t (row.cameraImageGeneration) - int64_t (row.imageGeneration);
            row.relation = row.delta == 0 ? Relation::Match : Relation::Mismatch;
        }
        CountRelation (row.relation, g_matched, g_mismatched, g_unknown, g_ambiguous);

        // UNIQUE = first successful Present whose handoffSerial differs from
        // the last unique's; REPEAT = same handoffSerial as the previous
        // Present. A zero handoffSerial (nothing has reached B yet) is neither.
        if (row.handoffSerial != 0) {
            if (row.handoffSerial != g_lastUniqueHandoffSerial) {
                g_lastUniqueHandoffSerial = row.handoffSerial;
                g_uniqueImagePassesObserved.fetch_add (1, std::memory_order_relaxed);
                CountRelation (row.relation, g_uniqueMatched, g_uniqueMismatched, g_uniqueUnknown, g_uniqueAmbiguous);
                if (row.relation == Relation::Match || row.relation == Relation::Mismatch) {
                    g_uniqueImagePassesClassified.fetch_add (1, std::memory_order_relaxed);
                    BucketDelta (g_uniqueDelta, row.delta);
                }
                PublishRow (row);
            }
            else {
                g_duplicatePresents.fetch_add (1, std::memory_order_relaxed);
                CountRelation (row.relation, g_repeatMatched, g_repeatMismatched, g_repeatUnknown, g_repeatAmbiguous);
                if (row.relation == Relation::Match || row.relation == Relation::Mismatch)
                    BucketDelta (g_repeatDelta, row.delta);
            }
        }
    }
    g_pendingPresent = Row {};
    g_pendingPresentActive = false;
    g_pendingCurrentModelGeneration = 0;
    g_pendingHandoffSameGeneration = false;
}

void OnResizeBuffers ()
{
    if (!Enabled ())
        return;
    ClearImagePublication ();
    g_pendingImage = ImageWitness {};
    g_pendingImageValid = false;
    g_lastUniqueHandoffSerial = 0;
    // g_sceneColour is left untouched: the next root commit detects whether S's
    // identity actually changed and resets the generation numbering itself.
    g_backBuffer.store (0, std::memory_order_release);
    g_resizeFenceSerial.store (NextEvent (), std::memory_order_release);
}

bool PendingPresentView (PresentView& out)
{
    if (!g_pendingPresentActive)
        return false;
    out.handoffSerial = g_pendingPresent.handoffSerial;
    out.imageGeneration = g_pendingPresent.imageGeneration;
    out.cameraImageGeneration = g_pendingPresent.cameraImageGeneration;
    out.cameraBound = g_pendingPresent.overlayCameraSerial != 0;
    return true;
}

size_t CopyRows (Row* out, size_t capacity)
{
    if (out == nullptr || capacity == 0)
        return 0;
    const uint64_t written = g_rowsWritten.load (std::memory_order_acquire);
    uint64_t first = g_rowsVisibleFrom.load (std::memory_order_acquire);
    if (written > kRowCapacity && first < written - kRowCapacity)
        first = written - kRowCapacity;
    if (written - first > capacity)
        first = written - capacity;
    size_t copied = 0;
    for (uint64_t index = first; index < written; ++index) {
        RowSlot& slot = g_rows[index % kRowCapacity];
        const uint64_t expected = index + 1;
        if (slot.published.load (std::memory_order_acquire) != expected)
            continue;
        Row row;
        row.provenanceEpoch = slot.provenanceEpoch.load (std::memory_order_relaxed);
        row.eventSerial = slot.eventSerial.load (std::memory_order_relaxed);
        row.presentSerial = slot.presentSerial.load (std::memory_order_relaxed);
        row.imagePass = slot.imagePass.load (std::memory_order_relaxed);
        row.imageRootEventSerial = slot.imageRootEventSerial.load (std::memory_order_relaxed);
        row.imageSourceResource = slot.imageSourceResource.load (std::memory_order_relaxed);
        row.imageModelGeneration = slot.imageModelGeneration.load (std::memory_order_relaxed);
        row.overlayCameraSerial = slot.overlayCameraSerial.load (std::memory_order_relaxed);
        row.cameraSourcePass = slot.cameraSourcePass.load (std::memory_order_relaxed);
        row.cameraSnapshotEventSerial = slot.cameraSnapshotEventSerial.load (std::memory_order_relaxed);
        row.cameraAdoptEventSerial = slot.cameraAdoptEventSerial.load (std::memory_order_relaxed);
        row.ambiguityEventSerial = slot.ambiguityEventSerial.load (std::memory_order_relaxed);
        row.cameraMetadataPass = slot.cameraMetadataPass.load (std::memory_order_relaxed);
        row.backBuffer = slot.backBuffer.load (std::memory_order_relaxed);
        row.delta = slot.delta.load (std::memory_order_relaxed);
        row.relation = Relation (slot.relation.load (std::memory_order_relaxed));
        row.imageOnBackBuffer = slot.imageOnBackBuffer.load (std::memory_order_relaxed);
        row.cameraCoherent = slot.cameraCoherent.load (std::memory_order_relaxed);
        row.presentContextOverlap = slot.presentContextOverlap.load (std::memory_order_relaxed);
        row.handoffSerial = slot.handoffSerial.load (std::memory_order_relaxed);
        row.imageGeneration = slot.imageGeneration.load (std::memory_order_relaxed);
        row.imageRooted = slot.imageRooted.load (std::memory_order_relaxed);
        row.cameraImageGeneration = slot.cameraImageGeneration.load (std::memory_order_relaxed);
        row.metadataStale = slot.metadataStale.load (std::memory_order_relaxed);
        row.backBufferState = BackBufferState (slot.backBufferState.load (std::memory_order_relaxed));
        if (slot.published.load (std::memory_order_acquire) == expected)
            out[copied++] = row;
    }
    return copied;
}

Stats GetStats ()
{
    Stats stats;
    stats.enabled = Enabled ();
    stats.provenanceEpoch = g_provenanceEpoch.load (std::memory_order_acquire);
    stats.imageDrawsCommitted = g_imageDrawsCommitted.load (std::memory_order_relaxed);
    stats.cameraSnapshots = g_cameraSnapshots.load (std::memory_order_relaxed);
    stats.cameraBindings = g_cameraBindings.load (std::memory_order_relaxed);
    stats.presents = g_presents.load (std::memory_order_relaxed);
    stats.matched = g_matched.load (std::memory_order_relaxed);
    stats.mismatched = g_mismatched.load (std::memory_order_relaxed);
    stats.unknown = g_unknown.load (std::memory_order_relaxed);
    stats.ambiguous = g_ambiguous.load (std::memory_order_relaxed);
    stats.uniqueImagePassesObserved = g_uniqueImagePassesObserved.load (std::memory_order_relaxed);
    stats.uniqueImagePassesClassified = g_uniqueImagePassesClassified.load (std::memory_order_relaxed);
    stats.uniqueMatched = g_uniqueMatched.load (std::memory_order_relaxed);
    stats.uniqueMismatched = g_uniqueMismatched.load (std::memory_order_relaxed);
    stats.uniqueUnknown = g_uniqueUnknown.load (std::memory_order_relaxed);
    stats.uniqueAmbiguous = g_uniqueAmbiguous.load (std::memory_order_relaxed);
    stats.duplicatePresents = g_duplicatePresents.load (std::memory_order_relaxed);
    const uint64_t written = g_rowsWritten.load (std::memory_order_acquire);
    const uint64_t visible = g_rowsVisibleFrom.load (std::memory_order_acquire);
    stats.rowsOverwritten = written - visible > kRowCapacity ? written - visible - kRowCapacity : 0;
    stats.handoffs = g_handoffs.load (std::memory_order_relaxed);
    stats.handoffsUnrooted = g_handoffsUnrooted.load (std::memory_order_relaxed);
    stats.handoffsSameGeneration = g_handoffsSameGeneration.load (std::memory_order_relaxed);
    stats.generationsWithMultipleRoots = g_generationsWithMultipleRoots.load (std::memory_order_relaxed);
    stats.sceneColourChanges = g_sceneColourChanges.load (std::memory_order_relaxed);
    stats.backBufferClears = g_backBufferClears.load (std::memory_order_relaxed);
    stats.backBufferOtherWrites = g_backBufferOtherWrites.load (std::memory_order_relaxed);
    stats.presentsWithoutBinding = g_presentsWithoutBinding.load (std::memory_order_relaxed);
    stats.imageGeneration = g_imageGeneration.load (std::memory_order_relaxed);
    for (size_t i = 0; i < kDeltaBucketCount; ++i) {
        stats.uniqueDelta[i] = g_uniqueDelta[i].load (std::memory_order_relaxed);
        stats.repeatDelta[i] = g_repeatDelta[i].load (std::memory_order_relaxed);
    }
    stats.repeatMatched = g_repeatMatched.load (std::memory_order_relaxed);
    stats.repeatMismatched = g_repeatMismatched.load (std::memory_order_relaxed);
    stats.repeatUnknown = g_repeatUnknown.load (std::memory_order_relaxed);
    stats.repeatAmbiguous = g_repeatAmbiguous.load (std::memory_order_relaxed);
    return stats;
}

} // namespace scenecamerapairing
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
