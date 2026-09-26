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
};

struct CameraWitness {
    uint64_t provenanceEpoch = 0;
    uint64_t snapshotEventSerial = 0;
    uint64_t cameraSerial = 0;
    uint64_t sourcePass = 0;
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
};

template<> struct Publication<CameraWitness> {
    std::atomic<uint64_t> version { 0 };
    std::atomic<uint64_t> provenanceEpoch { 0 };
    std::atomic<uint64_t> snapshotEventSerial { 0 };
    std::atomic<uint64_t> cameraSerial { 0 };
    std::atomic<uint64_t> sourcePass { 0 };
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
};

std::atomic<bool> g_enabled { false };
std::atomic<uint64_t> g_provenanceEpoch { 0 };
std::atomic<uint64_t> g_eventSerial { 0 };
std::atomic<uint64_t> g_contextOperationVersion { 0 };
std::atomic<uint32_t> g_contextOperations { 0 };
thread_local bool g_contextOperationActive = false;
Publication<ImageWitness> g_image;
Publication<CameraWitness> g_camera;
ImageWitness g_pendingImage;
bool g_pendingImageValid = false;
Row g_pendingPresent;
uint64_t g_pendingContextOperationVersion = 0;
bool g_pendingContextOverlap = false;
bool g_pendingPresentActive = false;
uint64_t g_pendingCurrentModelGeneration = 0;
uint64_t g_lastUniqueImagePass = 0;
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
        const uint64_t after = g_camera.version.load (std::memory_order_acquire);
        if (before == after) {
            camera = candidate;
            return candidate.provenanceEpoch != 0 && candidate.cameraSerial != 0 && candidate.sourcePass != 0;
        }
    }
    return false;
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
    g_pendingPresent = Row {};
    g_pendingPresentActive = false;
    g_pendingCurrentModelGeneration = 0;
    g_lastUniqueImagePass = 0;
    ClearImagePublication ();
    g_camera.version.fetch_add (1, std::memory_order_acq_rel);
    g_camera.provenanceEpoch.store (0, std::memory_order_relaxed);
    g_camera.snapshotEventSerial.store (0, std::memory_order_relaxed);
    g_camera.cameraSerial.store (0, std::memory_order_relaxed);
    g_camera.sourcePass.store (0, std::memory_order_relaxed);
    g_camera.version.fetch_add (1, std::memory_order_release);
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
    PublishCamera (camera);
    g_cameraSnapshots.fetch_add (1, std::memory_order_relaxed);
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
        if (overlap) {
            row.relation = Relation::Ambiguous;
            row.ambiguityEventSerial = NextEvent ();
        }
        else if (row.imagePass == 0 || row.imageRootEventSerial == 0 || !row.imageOnBackBuffer ||
                 row.overlayCameraSerial == 0 || row.cameraSourcePass == 0 ||
                 row.imageModelGeneration != g_pendingCurrentModelGeneration) {
            row.relation = Relation::Unknown;
        }
        else if (!row.cameraCoherent) {
            row.relation = Relation::Ambiguous;
            row.ambiguityEventSerial = row.cameraAdoptEventSerial;
        }
        else {
            row.delta = int64_t (row.cameraSourcePass) - int64_t (row.imagePass);
            row.relation = row.delta == 0 ? Relation::Match : Relation::Mismatch;
        }
        CountRelation (row.relation, g_matched, g_mismatched, g_unknown, g_ambiguous);

        if (row.imageOnBackBuffer && row.imagePass != 0 && row.imagePass != g_lastUniqueImagePass) {
            g_lastUniqueImagePass = row.imagePass;
            g_uniqueImagePassesObserved.fetch_add (1, std::memory_order_relaxed);
            CountRelation (row.relation, g_uniqueMatched, g_uniqueMismatched, g_uniqueUnknown, g_uniqueAmbiguous);
            if (row.relation == Relation::Match || row.relation == Relation::Mismatch)
                g_uniqueImagePassesClassified.fetch_add (1, std::memory_order_relaxed);
            PublishRow (row);
        }
        else if (row.imageOnBackBuffer && row.imagePass != 0) {
            g_duplicatePresents.fetch_add (1, std::memory_order_relaxed);
        }
    }
    g_pendingPresent = Row {};
    g_pendingPresentActive = false;
    g_pendingCurrentModelGeneration = 0;
}

void OnResizeBuffers ()
{
    if (!Enabled ())
        return;
    ClearImagePublication ();
    g_pendingImage = ImageWitness {};
    g_pendingImageValid = false;
    g_lastUniqueImagePass = 0;
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
    return stats;
}

} // namespace scenecamerapairing
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
