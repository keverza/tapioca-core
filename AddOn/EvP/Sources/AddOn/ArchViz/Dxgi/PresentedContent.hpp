#ifndef EVP_ARCHVIZ_DXGI_PRESENTEDCONTENT_HPP
#define EVP_ARCHVIZ_DXGI_PRESENTEDCONTENT_HPP

// ArchViz/Dxgi/PresentedContent -- which composited image each Present actually
// shows, read from the back buffer's pixels, against the overlay camera drawn on it.
// Bound by private/docs/architecture/diligent/OVERLAY-INVARIANTS.md: observation only;
// it never changes what is drawn or presented.
//
// WHY IT EXISTS (Stage 75). Stages 72-74 established from bookkeeping that the
// overlay binds the camera of the image Archicad last composited into the back
// buffer, and that this camera is the model draws' own. The overlay still trails.
// No stage measured the assumption underneath: that a repeat Present shows the
// buffer Archicad composited. The chain is DXGI_SWAP_EFFECT_SEQUENTIAL with two
// buffers, Archicad composites once per ~6 Presents, and run 26 found that nothing
// drawn at one Present survives into the next back buffer. If the two buffers
// alternate, every other Present shows the OLDER image with the NEWEST overlay
// camera drawn over it.
//
// Per Present, three full-width rows of buffer 0 are copied BEFORE any of our
// drawing and AFTER all of it, immediately before the Present is forwarded. Read
// back in Present order: a repeat Present whose BEFORE rows equal the previous
// Present's AFTER rows showed the same buffer; equal to the AFTER rows of the
// Present before that, it showed the other one. The image each Present shows is
// carried along that chain from the composites and compared with the camera the
// overlay drew at that Present. A few consecutive presented frames are also kept
// as pixels -- a centred crop, after our drawing -- to be looked at.
//
// ⚠️ ITS ASSUMPTIONS, AND WHERE EACH IS CHECKED (OVERLAY-INVARIANTS.md §7):
//   - Present shows buffer 0 as it stands when the call is forwarded: the DXGI
//     contract; the AFTER copy is the last thing before the forward.
//   - Only Archicad's composite writes buffer 0 between two Presents: the
//     pairing's handoff witness decides "composited", and any other write makes
//     the next Present UNIDENTIFIED, which is counted.
//   - The sampled rows change from one image to the next: `compositedChanged`
//     against `composited`, printed as a VALIDITY line.
//   - Records are compared only along an unbroken run of Present serials:
//     `chainBreaks`, `slotsBusy` and `readbackFailures` are printed.

#include <cstddef>
#include <cstdint>
#include <string>

struct IDXGISwapChain;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace presentedcontent {

constexpr size_t kSlots = 16;                // Presents in flight to the CPU
constexpr size_t kRows = 3;                  // rows sampled per copy: H/4, H/2, 3H/4
constexpr size_t kDeltaBuckets = 5;          // overlay camera minus displayed image: <=-2, -1, 0, +1, >=+2
constexpr size_t kFrames = 6;                // consecutive presented frames kept as pixels
constexpr uint64_t kFirstFramePresent = 200; // well into the window, once orbiting is under way
constexpr uint32_t kFrameWidth = 1024;       // the centred crop, full resolution
constexpr uint32_t kFrameHeight = 576;

// ⚠️ ORDER IS THE WIRE FORMAT: the OverlayRegression diagnostic names them in this order.
enum class Relation : uint32_t { Unknown = 0, Composited, SameBuffer, OtherBuffer, Undecidable, Unidentified };
constexpr size_t kRelations = 6;
constexpr size_t kHookSlots = 28; // ContextSlot::Count; the .cpp asserts it

// Stage 76 -- what the context hooks saw between the previous Present and this
// one, per Relation. Stage 75 found the image changing at ~95% of the Presents
// the pairing called repeats; if the hooks saw no draw before those Presents,
// the frames were drawn while the runtime had the hooks re-pointed.
struct CoverageStats {
    uint64_t presents = 0;
    uint64_t calls = 0;   // hooked Archicad calls
    uint64_t draws = 0;   // of those, draw calls
    uint64_t noDraws = 0; // Presents preceded by no hooked draw at all
    uint64_t repairs = 0; // slots put back, by either repair
};

struct Stats {
    bool enabled = false;
    uint64_t presentsSampled = 0;   // both copies issued
    uint64_t presentsProcessed = 0; // read back and classified, in Present order
    uint64_t slotsBusy = 0;         // no free slot at a Present: that Present is not sampled
    uint64_t readbacksPending = 0;  // DO_NOT_WAIT refusals; retried, not a fault
    uint64_t readbackFailures = 0;
    uint64_t createFailures = 0;
    uint64_t abandoned = 0;         // the target changed between BEFORE and AFTER
    uint64_t chainBreaks = 0;       // gaps in Present order; the history restarts there
    uint64_t targetChanges = 0;     // the back buffer's size or format changed in the window
    uint64_t composited = 0;        // a new composite since the previous Present
    uint64_t compositedChanged = 0; // ...whose BEFORE rows differ from the previous AFTER rows
    uint64_t repeats = 0;
    uint64_t repeatSame = 0;        // BEFORE == AFTER(k-1): the same buffer again
    uint64_t repeatOther = 0;       // BEFORE == AFTER(k-2) != AFTER(k-1): the other buffer
    uint64_t repeatUndecidable = 0; // AFTER(k-1) == AFTER(k-2): the two candidates look alike
    uint64_t repeatUnidentified = 0;
    uint64_t firstRepeats = 0; // the first decidable repeat after each composite
    uint64_t firstRepeatSame = 0;
    uint64_t firstRepeatOther = 0;
    uint64_t firstRepeatUnidentified = 0;
    uint64_t overlayBound = 0; // the overlay bound a camera at this Present
    uint64_t trueDelta[kDeltaBuckets] = {};
    uint64_t trueDeltaUnknown = 0;
    // How long each Present stayed the newest one presented, by its true delta.
    double screenSeconds[kDeltaBuckets] = {};
    double screenSecondsUnknown = 0.0;
    uint64_t bookkeepingDisagrees = 0; // the pairing's delta was 0 and the pixels say otherwise
    uint32_t framesReady = 0;
    CoverageStats coverage[kRelations];
    uint64_t presentsWithRepairAfterCalls = 0; // Presents at which the after-call repair was armed
    uint64_t slotRepairs[kHookSlots] = {};     // re-points put back during the window, per hooked slot
};

struct FrameInfo {
    uint64_t presentSerial = 0;
    double seconds = 0.0; // since the first kept frame
    uint64_t handoffSerial = 0;
    uint64_t imageGeneration = 0;       // the last composite's, per the pairing
    uint64_t cameraImageGeneration = 0; // the overlay camera's
    bool cameraBound = false;
    Relation relation = Relation::Unknown;
    bool displayedKnown = false;
    uint64_t displayed = 0;                       // the image the pixels say this Present showed
    uint32_t x = 0, y = 0, width = 0, height = 0; // the crop within the back buffer
    std::string pathBefore;                       // the same crop BEFORE our drawing: Archicad's pixels alone
    std::string path;                             // written by WriteFrames; empty until then
};

// MAIN THREAD. Disabled before the shared PassProvenance drain; Reset after it.
// Enabling allocates the frame memory, so the Present path never allocates.
// ⚠️ `repairAfterCalls` ARMS THE CONTEXT HOOK'S AFTER-CALL REPAIR FOR THIS WINDOW
// ONLY (Stage 76's A/B). Its lifetime is the window's: disabling always disarms
// it, so production can never inherit it from a diagnostic (OVERLAY-INVARIANTS §9).
void SetEnabled (bool enabled, bool repairAfterCalls = false);
bool Enabled ();
void Reset ();

// PRESENT THREAD, nominated chain, inside the active PassProvenance interval.
// Begin runs after the pairing's BeginPresent and before any overlay drawing;
// Forward runs after all of it, immediately before the Present is forwarded.
void OnPresentBegin (IDXGISwapChain* swapChain);
void OnPresentForward (IDXGISwapChain* swapChain);

Stats GetStats ();

// The hooked slot's name, for the per-slot repair counts.
const char* HookSlotName (size_t slot);

// MAIN THREAD, after the drain. Writes each kept frame once, as a 32-bit BMP in
// logs\frames, and copies the frame records out; returns how many.
size_t WriteFrames (uint64_t epoch, FrameInfo* out, size_t capacity);

} // namespace presentedcontent
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
