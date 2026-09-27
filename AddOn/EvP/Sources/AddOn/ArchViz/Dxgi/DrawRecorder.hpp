#ifndef EVP_ARCHVIZ_DXGI_DRAWRECORDER_HPP
#define EVP_ARCHVIZ_DXGI_DRAWRECORDER_HPP

// ArchViz/Dxgi/DrawRecorder -- every draw Archicad issues on its immediate
// context during a few Presents, recorded WITHOUT a camera lock, a learned pass
// or a census group. A temporary diagnostic for the camera-lock regression of
// 2026-09-26/27 (HANDOFF-OverlayPatch.md): since 19:14 that day no census group
// has read a perspective projection in b2, and the question it answers is
// structural -- are the model's draws missing from interception, reaching the
// census and being refused, or carrying their camera in a different layout.
//
// What a record holds, per draw: the draw call and its size, the thread and
// context, the vertex and pixel shader identities, the colour and depth targets
// with their texture descriptions, the viewport, and for b0..b3 the bound
// buffer, its size, the window (first/num constants) and the window's first 64
// bytes -- one 4x4 matrix -- read back from the GPU. Every binding is ASKED OF
// THE CONTEXT at the draw (OVERLAY-INVARIANTS section 11): nothing here reads
// the state tracker's pointers. The census's own admission rule is applied to
// those bindings and the reason recorded, with the scene-pass state the
// renderstate capture had at the draw.
//
// Nothing is classified here. The matrices go out raw and the diagnostic reads
// them, so the analysis can change without a rebuild.
//
// ⚠️ EACH WINDOW IS COPIED TWICE: BEFORE THE DRAW AND AFTER IT. With the editing
// plane hidden and the camera orbiting (2026-09-27 17:40), the census -- which
// copies before the draw -- read the model's 1512-index draw as a camera in 7 of
// 1802 readbacks, while at five still views this recorder, also copying before
// the draw, read view x projection there every time, and the pose agreement --
// which copies after the draw -- has only ever run with the plane shown. The
// second copy says whether the window a draw used is the window read before it.
//
// COST. Disarmed: one relaxed atomic load and one thread-local read per draw,
// one load per Present. Armed: a few dozen context queries and eight 64-byte GPU
// copies per draw, for at most `kMaxDraws` draws, then one DO_NOT_WAIT readback
// at a Present.
//
// OVERLAY-INVARIANTS.md binds this file.

#include <cstddef>
#include <cstdint>

struct ID3D11DeviceContext;
struct IDXGISwapChain;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace drawrecorder {

constexpr size_t kMaxDraws = 256;
constexpr size_t kWindows = 4;          // b0..b3
constexpr size_t kFloatsPerWindow = 16; // the first 64 bytes: one 4x4 matrix
constexpr uint32_t kMaxFrames = 8;

// Why the census would take or refuse this draw, from the bindings recorded.
enum class Reason : uint32_t {
    Admissible = 0,        // b1 and b2 bound as 16-constant windows
    CensusOff = 1,         // the census was not enabled
    ViewUnbound = 2,       // no b1
    ProjectionUnbound = 3, // no b2
    ViewWindow = 4,        // b1 bound, but not as 16 constants
    ProjectionWindow = 5,  // b2 bound, but not as 16 constants
};

struct DrawRecord {
    uint32_t sequence = 0;  // capture order
    uint32_t frame = 0;     // Presents since the capture began
    uint32_t kind = 0;      // census::DrawKind
    uint32_t count = 0;     // index or vertex count
    uint32_t instances = 0; // 1 unless instanced
    uint32_t thread = 0;
    uint64_t context = 0;
    uint64_t vertexShader = 0;
    uint64_t pixelShader = 0;
    uint64_t renderTarget = 0;
    uint64_t renderResource = 0;
    uint32_t renderWidth = 0, renderHeight = 0, renderFormat = 0, renderSamples = 0;
    uint64_t depthView = 0;
    uint64_t depthResource = 0;
    uint32_t depthWidth = 0, depthHeight = 0, depthFormat = 0;
    float viewport[4] = {}; // x, y, width, height
    uint64_t buffer[kWindows] = {};
    uint32_t bufferBytes[kWindows] = {};
    uint32_t firstConstant[kWindows] = {};
    uint32_t numConstants[kWindows] = {};
    uint32_t bytesCopied[kWindows] = {};
    float window[kWindows][kFloatsPerWindow] = {};
    // The same windows copied again once the draw returned, and whether the
    // context still had the binding read before it.
    uint32_t bytesCopiedAfter[kWindows] = {};
    bool bindingKept[kWindows] = {};
    float windowAfter[kWindows][kFloatsPerWindow] = {};
    // The renderstate capture's view of the pass at this draw.
    uint64_t scenePass = 0;
    uint64_t passColour = 0;
    bool passBoundaryHit = false;
    uint64_t signatureColour = 0;
    bool signatureLearned = false;
    uint64_t modelGeneration = 0;
    bool censusEnabled = false;
    Reason reason = Reason::CensusOff;
};

enum class State : uint32_t { Idle = 0, Armed = 1, Capturing = 2, Closing = 3, Done = 4 };

struct Status {
    State state = State::Idle;
    uint32_t modelFramesWanted = 0; // armed for a moving camera: redraws before the capture
    uint32_t framesWanted = 0;
    uint32_t framesSeen = 0;
    uint32_t draws = 0;   // records written
    uint32_t dropped = 0; // draws past `kMaxDraws`
    uint32_t readbacksPending = 0;
    uint32_t readbackFailures = 0;
    uint32_t createFailures = 0;
    uint64_t captures = 0; // completed since the process started
};

// MAIN THREAD. Record every Archicad draw of the next `frames` Presents
// (clamped to 1..kMaxFrames). Re-arming discards the previous capture.
//
// With `afterModelFrames`, the capture starts only once Archicad has redrawn the
// model that many times since arming -- which a still camera does not do -- so it
// records a MOVING camera. An orbit is a mouse drag, and a command cannot be run
// in the middle of one: it arms first, and the orbit starts the capture.
void Arm (uint32_t frames, uint32_t afterModelFrames = 0);
// MAIN THREAD. Stop recording; a capture in progress is discarded.
void Disarm ();
Status GetStatus ();
// MAIN THREAD, once `Done`: the records, in capture order.
size_t CopyRecords (DrawRecord* out, size_t capacity);

// RENDER THREAD, from every Archicad draw detour, BEFORE the original draw.
void OnDraw (ID3D11DeviceContext* context, uint32_t kind, uint32_t count, uint32_t instances);
// RENDER THREAD, from the same detour AFTER the original draw: the second copy.
// A record stays in flight between the two, so the readback waits for it.
void OnDrawCompleted (ID3D11DeviceContext* context);
// PRESENT THREAD, for the nominated chain: frame boundaries and the readback.
void OnPresent (IDXGISwapChain* swapChain);

// Releases the staging buffer. Call with the hooks out.
void Shutdown ();

} // namespace drawrecorder
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
