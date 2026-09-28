#ifndef EVP_ARCHVIZ_DXGI_PLANOVERLAYLAYER_HPP
#define EVP_ARCHVIZ_DXGI_PLANOVERLAYLAYER_HPP

// ArchViz/Dxgi/PlanOverlayLayer -- the floor-plan overlay, drawn into Archicad's own
// back buffer at the plan's Present, through the transform read at that Present.
// Bound by private/docs/architecture/diligent/OVERLAY-INVARIANTS.md: §2, §11, §12 and
// finding 14.
//
// ⚠️ THE 3D OVERLAY'S WHERE AND WHEN, NEVER ITS CAMERA. Finding 14: every ACAPI read
// taken before the plan's Present is a frame stale, and the read taken inside it is
// the frame's transform (p95 0.50 px against the frame's own pixels). So the layer
// composes where the 3D overlay composes -- in the Present detour, before the frame is
// forwarded -- and asks the runtime for the transform at that moment, which §11
// permits on this chain alone. Nothing here recognises a GPU camera: the plan has none
// (finding 13), and 3D and plan share no camera logic (§12). The plan presents full
// frames through plain Present -- never Present1, no dirty or scroll rects (the frame
// record, 2026-09-28) -- so a full-buffer draw is a visible one.
//
// ⚠️ THE PRESENT CREATES ONE THING: THE BACK BUFFER'S VIEW, released before it returns,
// because a view held across a frame makes ResizeBuffers fail (§11). Shaders, states
// and the segment buffer are built by `Prepare`, on the main thread OUTSIDE any
// Present, for the device the plan's chain presented with -- learned and held from its
// first Present, as the 3D path holds its device (§12b). The plan's Present may not
// lock or allocate (§11), and a first-use compile inside it would do both.
//
// ⚠️ A FRAME WITHOUT A FRESH READ IS DRAWN WITH THE LAST ONE, NEVER LEFT BARE. A still
// plan does not present again, so a frame presented without the overlay keeps it off
// the screen until the user next moves the view. When a read may not be taken -- the
// runtime is inside an ACAPI call of its own, whose redraw moves nothing -- or is taken
// and fails, the frame is drawn with the last transform read, and counted (§7). The
// read is taken on EVERY canvas Present, drawn or not, so the last one is never older
// than the frame before.
//
// THREADS. `OnPresent` runs in both Present detours on every chain's thread: it leaves
// at its second test on any chain but the plan canvas's own, and draws only on the
// thread it was armed with, which is ACAPI's. Everything else is MAIN THREAD.

#include "ArchViz/PlanOverlayContent.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

struct IDXGISwapChain;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace planlayer {

// What asking for the frame's transform came to.
enum class Read : uint32_t {
    Fresh = 0, // read now: this frame's transform
    Refused,   // may not be read now (the runtime is inside an ACAPI call of its own)
    Invalid,   // read and failed: an ACAPI error, or the view moved between the samples
};

// Asked inside the plan's Present, on ACAPI's thread: model metres -> the back buffer's
// PHYSICAL pixels. `error` is ACAPI's when a sample was refused.
using TransformReader = Read (*) (plancontent::PixelTransform& out, int32_t& error);

struct Style {
    float red = 1.0f;
    float green = 0.42f;
    float blue = 0.0f;
    float alpha = 0.9f;
    float widthPixels = 2.0f; // physical pixels
};

// MAIN THREAD. Arm for one canvas and the thread its chain presents on. Every counter
// and the last transform are reset (§8); what was built survives, and is rebuilt only
// if the chain turns out to present with another device.
void Arm (uint64_t canvasWindow, uint32_t presentThread, TransformReader reader, const Style& style);

// MAIN THREAD. Nothing is drawn from the next Present on.
void Disarm ();

// MAIN THREAD. The plan's canvas is another window now; its chain is matched from the
// next Present. Counters and the last transform are kept: the view did not change.
void Retarget (uint64_t canvasWindow);

// MAIN THREAD, outside any Present. A transform read before the first Present, so the
// first frame has one even if its own read is refused.
void SeedTransform (const plancontent::PixelTransform& transform);

enum class Prepared : uint32_t {
    Ready = 0,        // the pipeline stands for the chain's device and holds `generation`
    WaitingForDevice, // no Present has named the device yet (or it changed and was let go)
    Failed,           // the pipeline could not be built for this device; `error` says why
};

// MAIN THREAD, outside any Present. Build the pipeline for the device the plan's chain
// presented with, and upload `content` when `generation` is not the one it holds.
// `changed` is true when anything new will be drawn -- the caller redraws for it, since
// a still plan would otherwise show it only when next moved.
Prepared Prepare (const plancontent::Content& content, uint64_t generation, bool& changed, std::string& error);

// MAIN THREAD. Every D3D object and the held device. Only once no Present can reach the
// layer's resources: after `Disarm` on this thread, or after the hook is gone.
void Release ();

// PRESENT DETOUR, both of them, every chain. Before the frame is forwarded.
void OnPresent (IDXGISwapChain* swapChain);

// Why a Present on the canvas's chain was not drawn. Each has its own counter (§7).
enum class Decline : uint32_t {
    OffThread = 0, // the canvas's chain presented on a thread that is not ACAPI's
    NoDevice,      // the chain would not name its device
    DeviceChanged, // not the device the pipeline was built for; the runtime rebuilds
    NotPrepared,   // no pipeline for this device yet
    NoContent,     // nothing to draw on this storey
    NoTransform,   // no read has succeeded yet
    Degenerate,    // the transform does not invert, so the view has no anchor
    BackBuffer,    // GetBuffer refused
    Multisampled,  // a multisampled back buffer; not drawn into
    NoContext1,    // no D3D11.1 context: the constant buffers could not be put back
    CreateView,    // CreateRenderTargetView refused the back buffer
    MapConstants,  // the constant buffer would not map
    Count
};
const char* DeclineName (Decline decline);

// Cumulative since Arm: the runtime reports deltas (§7).
struct Stats {
    bool armed = false;
    bool deviceKnown = false;
    bool prepared = false;
    uint64_t chain = 0;
    uint32_t bufferWidth = 0;
    uint32_t bufferHeight = 0;
    uint32_t segments = 0;
    uint64_t generation = 0;
    uint64_t canvasPresents = 0;
    uint64_t drawn = 0;
    uint64_t readsFresh = 0;
    uint64_t readsRefused = 0;
    uint64_t readsInvalid = 0;
    uint64_t drawnWithLastRead = 0;
    int32_t lastReadError = 0;
    uint64_t declines[size_t (Decline::Count)] = {};
    uint32_t readUsLast = 0;
    uint32_t readUsMax = 0; // since the last TakeMaxima
    uint32_t drawUsLast = 0;
    uint32_t drawUsMax = 0;
};
Stats GetStats ();

// MAIN THREAD. Restart the two maxima, so each report's worst case is its own interval's.
void TakeMaxima ();

} // namespace planlayer
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
