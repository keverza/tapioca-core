#ifndef EVP_ARCHVIZ_DXGI_IMAGETRANSFERTRACE_HPP
#define EVP_ARCHVIZ_DXGI_IMAGETRANSFERTRACE_HPP

// ArchViz/Dxgi/ImageTransferTrace -- Stage 70 measured that the verified scene
// colour is offscreen and the nominated back buffer is a different resource.
// This diagnostic follows, per bounded capture, which host GPU operation(s)
// carry that image into the back buffer, with what bound source and in what
// order relative to Present -- observation only, nothing here changes.
// Bound by private/docs/architecture/diligent/OVERLAY-INVARIANTS.md: it must
// never gate camera selection, composition or lineage classification, and a
// bound SRV is reported only as a candidate input, never as proven sampling.

#include "ArchViz/Dxgi/PassProvenance.hpp"

#include <cstddef>
#include <cstdint>

struct ID3D11Resource;
struct IDXGISwapChain;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace imagetransfer {

constexpr size_t kMaxCaptures = 4;
constexpr size_t kEventsPerCapture = 96;
constexpr size_t kTrackedPerCapture = 8;
constexpr size_t kPresentsPerCapture = 3;
constexpr size_t kCaptureSpacing = 8;

enum class Role : uint32_t { Context = 0, Present = 1 };

enum class Kind : uint32_t {
    Root = 0,
    NextRoot = 1,
    Draw = 2,
    Copy = 3,
    PartialCopy = 4,
    Clear = 5,
    ResourceWrite = 6,
    UnmodelledWork = 7,
    PresentBegin = 8,
    PresentEnd = 9
};

enum class CloseReason : uint32_t { Open = 0, Presents = 1, Full = 2, Resize = 3 };

// One recorded operation, published per slot while a capture is open. Only the
// fields the recording rule for `kind` fills are meaningful; the rest keep
// their default. `drawKind`/`drawCount` are Draw-only; `source`/`destination`
// are Copy/PartialCopy-only; `resource` is Clear/ResourceWrite-only.
struct Event {
    uint64_t epoch = 0;
    uint32_t capture = 0;
    uint64_t orderSerial = 0;
    Role role = Role::Context;
    Kind kind = Kind::Root;
    passprovenance::DrawKind drawKind = passprovenance::DrawKind::Unknown;
    uint32_t drawCount = 0;
    uint64_t drawsSinceRoot = 0;
    uint64_t scenePass = 0;
    uint64_t rtv0 = 0;
    uint32_t rtvCount = 0;
    uint64_t srv0 = 0;
    uint64_t srv1 = 0;
    uint64_t srv2 = 0;
    uint64_t srv3 = 0;
    uint32_t trackedSrvMask = 0;
    uint32_t trackedSrvHits = 0;
    uint64_t source = 0;
    uint64_t destination = 0;
    uint64_t resource = 0;
    uint64_t backBuffer = 0;
    bool writesBackBuffer = false;
    bool readsTracked = false;
    bool readsRoot = false;
    uint64_t trackedResource = 0;
    uint32_t trackedAdded = 0;
    uint64_t parentResource = 0;
    bool succeeded = false;
};

// One capture's summary, published at close. An open one (never closed by the
// time the main thread drains) reports `closeReason == CloseReason::Open`.
struct Capture {
    uint32_t capture = 0;
    uint64_t rootPass = 0;
    uint64_t rootResource = 0;
    uint64_t rootEventSerial = 0;
    uint64_t openOrderSerial = 0;
    uint64_t closeOrderSerial = 0;
    CloseReason closeReason = CloseReason::Open;
    uint32_t presentsSeen = 0;
    uint64_t drawsSinceRoot = 0;
    uint64_t rootWrites = 0;
    uint64_t unmodelledWork = 0;
    uint32_t trackedCount = 0;
    uint64_t trackedOverflow = 0;
    uint32_t eventsRecorded = 0;
    uint64_t eventsDropped = 0;
};

struct Stats {
    bool enabled = false;
    uint64_t epoch = 0;
    uint32_t capturesOpened = 0;
    uint32_t capturesClosed = 0;
    uint32_t capturesTruncated = 0;
    uint32_t capturesAbortedByResize = 0;
    uint64_t rootsSeen = 0;
    uint64_t eventsRecorded = 0;
    uint64_t eventsDropped = 0;
    uint64_t eventsAfterClose = 0;
    uint64_t unmodelledWork = 0;
    uint64_t trackedOverflow = 0;
    uint64_t lastBackBuffer = 0;
};

// MAIN THREAD. Mirrors passprovenance/scenecamerapairing: the owning command
// disables this before draining the shared PassProvenance callback gate.
void SetEnabled (bool enabled);
bool Enabled ();
void Reset ();
size_t CopyEvents (Event* out, size_t capacity);
size_t CopyCaptures (Capture* out, size_t capacity);
Stats GetStats ();

// CONTEXT THREAD, called only from inside functions PassProvenance.cpp and
// SceneCameraPairing.cpp already run under their own context-thread ownership,
// so this module reinvents no thread check of its own (OVERLAY-INVARIANTS.md
// risk 1). Tracked set and the one-shot root-consumption flag are plain data,
// safe because only this one thread ever touches them.
void OnImageCommitted (uint64_t scenePass, uint64_t modelGeneration, uint64_t sceneColorResource,
                       uint64_t rootEventSerial);
void OnDraw (passprovenance::DrawKind drawKind, uint32_t drawCount, const uint64_t* rtvs, size_t rtvSlots,
             const uint64_t* srvs, size_t srvSlots);
void OnCopy (uint64_t destination, uint64_t source);
void OnPartialCopy (uint64_t destination, uint64_t source);
void OnClear (uint64_t resource);
void OnResourceWrite (uint64_t resource);
void OnUnmodelledWork ();

// PRESENT THREAD. Only called for the nominated chain while PassProvenance is
// active for that Present (`passProvenanceActive`). `BeginPresent` refreshes
// the published back-buffer identity on every such call, whether or not a
// capture is open; only event recording is conditional on one being open.
bool BeginPresent (IDXGISwapChain* swapChain);
void EndPresent (bool succeeded);
void OnResizeBuffers ();

} // namespace imagetransfer
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
