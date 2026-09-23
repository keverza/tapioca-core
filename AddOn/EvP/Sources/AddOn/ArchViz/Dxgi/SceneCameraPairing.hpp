#ifndef EVP_ARCHVIZ_DXGI_SCENECAMERAPAIRING_HPP
#define EVP_ARCHVIZ_DXGI_SCENECAMERAPAIRING_HPP

// Build-specific pairing of an independently verified model image with the
// exact camera snapshot buffers bound by the Present overlay renderer. A
// primary row exists only when the verified draw resource is the backbuffer
// fetched for that Present; generic resource propagation is not accepted.

#include <cstddef>
#include <cstdint>

struct IDXGISwapChain;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace scenecamerapairing {

constexpr size_t kRowCapacity = 128;

enum class Relation : uint32_t { Unknown = 0, Match = 1, Mismatch = 2, Ambiguous = 3 };

struct Row {
    uint64_t provenanceEpoch = 0;
    uint64_t eventSerial = 0;
    uint64_t presentSerial = 0;
    uint64_t imagePass = 0;
    uint64_t imageRootEventSerial = 0;
    uint64_t imageSourceResource = 0;
    uint64_t imageModelGeneration = 0;
    uint64_t overlayCameraSerial = 0;
    uint64_t cameraSourcePass = 0;
    uint64_t cameraSnapshotEventSerial = 0;
    uint64_t cameraAdoptEventSerial = 0;
    uint64_t ambiguityEventSerial = 0;
    uint64_t cameraMetadataPass = 0;
    uint64_t backBuffer = 0;
    int64_t delta = 0;
    Relation relation = Relation::Unknown;
    bool imageOnBackBuffer = false;
    bool cameraCoherent = false;
    bool presentContextOverlap = false;
};

struct Stats {
    bool enabled = false;
    uint64_t provenanceEpoch = 0;
    uint64_t imageDrawsCommitted = 0;
    uint64_t cameraSnapshots = 0;
    uint64_t cameraBindings = 0;
    uint64_t presents = 0;
    uint64_t matched = 0;
    uint64_t mismatched = 0;
    uint64_t unknown = 0;
    uint64_t ambiguous = 0;
    uint64_t uniqueImagePassesObserved = 0;
    uint64_t uniqueImagePassesClassified = 0;
    uint64_t uniqueMatched = 0;
    uint64_t uniqueMismatched = 0;
    uint64_t uniqueUnknown = 0;
    uint64_t uniqueAmbiguous = 0;
    uint64_t duplicatePresents = 0;
    uint64_t rowsOverwritten = 0;
};

// MAIN THREAD. The owning command disables this before draining the shared
// pass-provenance callback gate, then reads the bounded rows.
void SetEnabled (bool enabled);
bool Enabled ();
void Reset ();

// CONTEXT THREAD. The operation interval is shared with PassProvenance so a
// Present that races host GPU work remains inconclusive.
bool BeginContextOperation ();
void EndContextOperation ();
void ArmVerifiedModelDraw (uint64_t scenePass, uint64_t modelGeneration, uint64_t targetEpoch,
                           uint64_t drawSequence, uint64_t sceneColorResource);
void OnDrawCompleted ();
void OnCameraSnapshot (uint64_t cameraSerial, uint64_t sourcePass);

// PRESENT THREAD. `OnOverlayCameraBound` is called at the literal snapshot
// buffer binding, not from latest camera state sampled later.
bool BeginPresent (IDXGISwapChain* swapChain, uint64_t currentModelGeneration);
void OnOverlayCameraBound (uint64_t metadataPass);
void EndPresent (bool active, bool succeeded);
void OnResizeBuffers ();

size_t CopyRows (Row* out, size_t capacity);
Stats GetStats ();

} // namespace scenecamerapairing
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
