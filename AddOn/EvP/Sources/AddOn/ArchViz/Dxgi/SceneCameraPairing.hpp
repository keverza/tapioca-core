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
constexpr size_t kDeltaBucketCount = 7; // {<=-3,-2,-1,0,+1,+2,>=+3}

enum class Relation : uint32_t { Unknown = 0, Match = 1, Mismatch = 2, Ambiguous = 3 };

// The nominated back buffer B as observed by `OnHostDraw`/`OnClearRenderTarget`/
// `OnResourceWritten`: whether the last thing to touch it was the verified
// composite (Handoff), a clear with no following composite (Cleared), some
// other write layered on top of a composite (Mixed), or nothing yet (None).
enum class BackBufferState : uint32_t { None = 0, Handoff = 1, Cleared = 2, Mixed = 3 };

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
    bool imageOnBackBuffer = false; // reported only; no longer gates classification
    bool cameraCoherent = false;
    bool presentContextOverlap = false;
    // Stage 72 -- the image handoff pairing. `imageGeneration` is the image
    // generation carried by the handoff that put an image on B, never the root
    // witness's; `cameraImageGeneration` is the same counter as read off the
    // overlay's bound camera. `delta = cameraImageGeneration - imageGeneration`.
    uint64_t handoffSerial = 0;
    uint64_t imageGeneration = 0;
    bool imageRooted = false;
    uint64_t cameraImageGeneration = 0;
    bool metadataStale = false; // reported only, never a classification input
    BackBufferState backBufferState = BackBufferState::None;
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
    // Stage 72 -- the image handoff pairing (see Row above).
    uint64_t handoffs = 0;
    uint64_t handoffsUnrooted = 0;
    uint64_t handoffsSameGeneration = 0;
    uint64_t generationsWithMultipleRoots = 0;
    uint64_t sceneColourChanges = 0;
    uint64_t backBufferClears = 0;
    uint64_t backBufferOtherWrites = 0;
    uint64_t presentsWithoutBinding = 0;
    uint64_t imageGeneration = 0; // current value, a count of clears of S -- not a delta
    uint64_t uniqueDelta[kDeltaBucketCount] = {};
    uint64_t repeatMatched = 0;
    uint64_t repeatMismatched = 0;
    uint64_t repeatUnknown = 0;
    uint64_t repeatAmbiguous = 0;
    uint64_t repeatDelta[kDeltaBucketCount] = {};
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

// CONTEXT THREAD, forwarded from PassProvenance.cpp's own already-gated
// per-draw handlers (every draw/clear/copy/write, not only the verified one),
// so B's actual composite draw -- a separate draw from the verified model
// draw that renders into S -- is seen here. `rtvs`/`srvs` are the same fixed
// per-slot arrays PassProvenance already tracks; slots holding 0 are unbound.
void OnHostDraw (const uint64_t* rtvs, size_t rtvSlots, const uint64_t* srvs, size_t srvSlots);
void OnClearRenderTarget (uint64_t resource);
void OnResourceWritten (uint64_t resource);

// CONTEXT THREAD. The current image generation (clears of S; 0 = unknown) and the
// number of verified-draw commits so far. CameraAgreement reads both in the same
// draw's PostDraw, after `OnDrawCompleted` above has committed that draw's root.
uint64_t ContextImageGeneration ();
uint64_t ContextRootCommits ();

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
