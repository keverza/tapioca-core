#ifndef EVP_ARCHVIZ_DXGI_PLANFRAMERECORD_HPP
#define EVP_ARCHVIZ_DXGI_PLANFRAMERECORD_HPP

// ArchViz/Dxgi/PlanFrameRecord -- the Present half of the floor-plan frame record:
// every Present of every swap chain while the plan is in front, and the pixels of
// the plan's own frames. Bound by private/docs/architecture/diligent/
// OVERLAY-INVARIANTS.md: observation only, it draws nothing and changes nothing
// that is presented.
//
// THE QUESTION IT SERVES (HANDOFF-OverlayPatch.md, "NEXT SESSION: floor plan
// overlay sync"). Where to compose the plan overlay -- our own window over the
// canvas, or Archicad's plan frame at Present -- depends on facts nobody has
// measured: which chain the plan presents through, on which thread, inside which
// of the main thread's messages, and whether the model-to-pixel transform ACAPI
// reports at that moment is the one the frame was drawn with. The main thread's
// half (ArchViz/PlanFrameSession) samples the transform and publishes what it is
// doing; this half stamps each Present with it and keeps the target chain's
// pixels, from which ArchViz/PlanFrameRegistration measures what each frame
// really shows.
//
// PER PRESENT, EVERY CHAIN: the time, the chain, the thread, Present or Present1
// with its dirty and scroll rectangles, and the main thread's state at that
// instant -- the plan canvas message being handled (if any), the newest transform
// sample, the last message the main thread retrieved.
//
// PER PRESENT, THE PLAN'S CHAIN: a centred crop of buffer 0 BEFORE the Present is
// forwarded, copied to a staging texture and read back DO_NOT_WAIT at a later
// Present, then kept as a half-resolution grey image.
//
// ⚠️ THE PRESENT PATH NEVER ALLOCATES, LOCKS OR CALLS ACAPI (§11). Every buffer is
// allocated by Arm on the main thread; staging textures are created on the
// Present thread from the chain's own device, as PresentedContent does. The
// device, context and back buffer are asked for per Present and released before
// it returns -- nothing of Archicad's is held across a frame.

#include <cstddef>
#include <cstdint>
#include <string>

struct IDXGISwapChain;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace planframes {

constexpr size_t kMaxPresents = 8192;
constexpr size_t kMaxFrames = 240;
constexpr uint32_t kCropWidth = 1024; // physical pixels of the back buffer, centred
constexpr uint32_t kCropHeight = 768;
constexpr uint32_t kDownsample = 2; // a kept sample is a 2x2 average: two physical pixels
constexpr uint32_t kFrameWidth = kCropWidth / kDownsample;
constexpr uint32_t kFrameHeight = kCropHeight / kDownsample;
constexpr size_t kStagingSlots = 6;

struct PresentRecord {
    int64_t qpc = 0;
    uint64_t chain = 0;
    // The main thread at this instant, as it last published itself.
    uint64_t canvasSerial = 0;    // the plan canvas message being handled, by the serial of its entry; 0 none
    uint64_t latestSample = 0;    // the newest transform sample published
    uint64_t retrievedSerial = 0; // the last message the main thread retrieved
    uint64_t retrievedWindow = 0;
    uint32_t thread = 0;
    uint32_t flags = 0; // DXGI_PRESENT_*
    uint32_t syncInterval = 0;
    uint32_t dirtyRects = 0; // Present1 only
    int32_t scrollX = 0;     // Present1's scroll offset, when it has one
    int32_t scrollY = 0;
    uint32_t canvasDepth = 0;
    uint32_t canvasMessage = 0;
    uint32_t retrievedMessage = 0;
    uint32_t tookUs = 0;       // our own capture work at this Present
    int32_t frame = -1;        // the frame kept for this Present, or -1
    uint32_t targetSerial = 0; // this chain's Presents since arming, for the target chain
    uint64_t atPresent = 0;    // the read of the transform taken INSIDE this Present, by serial; 0 none
    bool present1 = false;
    bool scroll = false;
    bool target = false;
    bool mainThread = false;
};

struct FrameInfo {
    uint32_t presentIndex = 0;
    uint32_t targetSerial = 0; // consecutive frames of the plan have consecutive serials
    int64_t qpc = 0;
    uint32_t cropX = 0; // the crop's top-left in the back buffer, physical pixels
    uint32_t cropY = 0;
    uint32_t width = 0; // the kept image, samples
    uint32_t height = 0;
    uint32_t bufferWidth = 0;
    uint32_t bufferHeight = 0;
    uint32_t format = 0;
};

struct Stats {
    bool armed = false;
    bool capturing = false;
    uint64_t target = 0;
    uint64_t presents = 0; // records written, every chain
    uint64_t presentsDropped = 0;
    uint64_t targetPresents = 0;
    uint64_t framesSubmitted = 0;
    uint64_t framesReady = 0;
    uint64_t slotsBusy = 0; // a target Present with no free staging slot: its frame is not kept
    uint64_t readbacksPending = 0;
    uint64_t readbackFailures = 0;
    uint64_t createFailures = 0;
    uint64_t unsupportedFormat = 0;
    uint64_t targetChanges = 0; // the plan's buffer changed size or format mid-record
    uint32_t format = 0;
};

// MAIN THREAD. What the main thread is doing, for the Present path to stamp.
void PublishCanvasMessage (uint32_t depth, uint32_t message, uint64_t serial);
void PublishLatestSample (uint64_t serial);
void PublishRetrievedMessage (uint32_t message, uint64_t window, uint64_t serial);

// MAIN THREAD. A read of the plan's transform to take INSIDE the plan's own Present,
// returning its serial (0 when none was taken); nullptr stops it.
//
// ⚠️ AN ACAPI READ INSIDE A PRESENT DETOUR, AND ONLY THIS ONE. Section 11 keeps ACAPI
// out of hot hooks because the 3D Present runs on a render thread, where ACAPI is
// illegal. The plan presents on the MAIN thread -- 470 of 470 Presents, 2026-09-28 --
// inside Archicad's own paint pass, and finding 14 leaves a read there as the one
// candidate for the frame's transform. So the reader is called only for the plan's
// chain, only while the record is capturing, and only when the calling thread is the
// main thread, checked on every call.
using PresentReader = uint64_t (*) ();
void SetPresentReader (PresentReader reader);

// MAIN THREAD. Allocate everything and start recording. `mainThread` is the id
// ACAPI runs on, so every record can say whether it was presented there.
bool Arm (uint32_t mainThread, std::string& error);
// MAIN THREAD. The plan's chain, once the session has identified it.
void SetTarget (uint64_t swapChain);
uint64_t Target ();
// MAIN THREAD. Stop recording new Presents and frames; frames already copied are
// still read back at the target's next Presents.
void StopCapturing ();
// MAIN THREAD. Stop everything. Call BEFORE the Present hook is removed; the
// removal's drain is what makes the buffers safe to read.
void Disarm ();
bool Armed ();
Stats GetStats ();

// MAIN THREAD, once disarmed and the Present hook is out.
size_t CopyPresents (PresentRecord* out, size_t capacity);
size_t FrameCount (); // frames read back and ready
bool CopyFrame (size_t index, FrameInfo& info, const uint8_t*& pixels);
// Release the staging textures and, with `memory`, the frame store.
void Release (bool memory);

// PRESENT THREAD, from both detours, before the Present is forwarded.
void OnPresent (IDXGISwapChain* swapChain, bool present1, uint32_t syncInterval, uint32_t flags, uint32_t dirtyRects,
                bool scroll, int32_t scrollX, int32_t scrollY);

} // namespace planframes
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
