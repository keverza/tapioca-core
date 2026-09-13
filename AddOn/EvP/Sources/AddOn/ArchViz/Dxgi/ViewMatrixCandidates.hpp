#ifndef EVP_ARCHVIZ_DXGI_VIEWMATRIXCANDIDATES_HPP
#define EVP_ARCHVIZ_DXGI_VIEWMATRIXCANDIDATES_HPP

// Finding ARCHICAD'S OWN view-projection in the constant buffers it uploads
// (PLAT-RE155, docs/architecture/api/HANDOFF-OverlayPatch.md stage 3).
// DISCOVERY ONLY -- nothing here is on a rendering path.
//
// WHY THIS IS THE STAGE THAT DECIDES THE RUNG. Stages 1 and 2 produce plumbing;
// this is where the premise is either confirmed or refuted. If a 64-byte region
// of one of Archicad's constant buffers can be shown to hold the same transform
// Archicad rendered the frame with, then the overlay can stop reconstructing a
// camera it learns about too late and start reading the one that was actually
// used. If no region matches, no amount of hooking helps and the ladder is over.
//
// ⚠️ WE HAVE A KNOWN-GOOD CAMERA AND THAT CHANGES THE METHOD ENTIRELY. An
// earlier draft of the plan proposed projecting known world points blindly
// through every candidate matrix and eyeballing which looked plausible. There is
// something far stronger available: `ACAPI_View_Get3DProjectionSets` gives eye,
// target and cone, `MatrixMath` already builds a projection from them that
// agrees with Archicad's rendering to within the residual this whole ladder is
// chasing, and AT REST that residual is zero because nothing is moving. So a
// candidate is scored against a reference, in pixels, on a still view -- a
// number, not a judgement.
//
// ⚠️ SCORED AT REST, AND ONLY AT REST. The reference is stale by construction
// while the view is moving -- that staleness is the entire problem this rung
// exists to remove -- so a candidate scored mid-drag would be penalised for
// being RIGHT. `SetReference` is told whether the camera moved, and scoring only
// consumes samples from a settled view.
//
// ⚠️ THE SCORE IS IN PIXELS AND IGNORES DEPTH, deliberately. Archicad's renderer
// may be left-handed, may clip depth to [0,1] or [-1,1], and may use a reversed
// depth buffer; none of that changes where a pixel lands, and all of it would
// make an element-by-element matrix comparison meaningless. What must agree is
// the screen position of a world point, which is what the overlay draws with.
// Depth convention is stage 6's problem, not this file's.
//
// ⚠️ FOUR VARIANTS PER REGION, BECAUSE STORAGE ORDER IS NOT OBSERVABLE. Sixteen
// floats in a buffer are a matrix in one of two orders, and the buffer may hold
// the transform or its inverse -- so each region is scored as M, its transpose,
// and the inverse of each. `MatrixMath` is row-vector/row-major, and an HLSL
// `float4x4` packs column-major by default, so THE TRANSPOSE IS THE EXPECTED
// HIT; the other three are there so that finding one of them is information
// rather than a dead end.
//
// ⚠️ HOW OFTEN A BUFFER CHANGES IS AS DISCRIMINATING AS WHAT IS IN IT. The
// handoff asks for regions "constant at idle, changing on orbit / pan / zoom,
// unchanged when only geometry changes", and that is cheap to record: every
// write is counted against whether the camera was moving at the time. A region
// that changes while the view is still is not a view matrix however well it
// scores on one frame, and a region that never changes during an orbit cannot
// be one at all.
//
// THREAD SAFETY. `OnConstantBufferBound` and `OnConstantBufferWrite` run inside
// the context detours on Archicad's render thread: they allocate nothing, take
// no lock and never call ACAPI. Everything else is main thread. The byte
// payloads cross that boundary through a per-entry sequence counter (odd while
// being written), so a reader that catches a write in progress retries instead
// of scoring half of one upload and half of the next.

#include <cstdint>

struct ID3D11Buffer;
struct ID3D11Resource;

namespace geomsrv {
namespace archviz {
namespace dxgi {

namespace renderstate {
struct GpuViewport;
}

namespace viewmatrix {

// ⚠️ THIS IS A WINDOW SIZE, NOT A BUFFER SIZE LIMIT, and the distinction is the
// whole of what the tenth live run bought. It was written as a buffer limit --
// "a view-projection lives in a small per-frame buffer; anything larger is a
// material array" -- and that assumption cost stage 3 two runs.
//
// ⚠️ ARCHICAD 29 PACKS ITS CONSTANTS INTO ONE 8 MiB RING AND BINDS WINDOWS OF
// IT. Run ten: 18110 of 34092 maps were constant buffers and every one was
// refused for being too large, with the widest at 8388608 bytes -- while
// `VSSetConstantBuffers1` fired 27216 times, which is the D3D11.1 call that
// exists precisely to bind a sub-range of a larger buffer. Raising a limit to
// 8 MiB would have copied eight megabytes of write-combined memory per map on
// Archicad's render thread; the camera would still not have been at offset zero.
//
// So the capture follows the bound offset instead: `firstConstant * 16` says
// where the window starts and this says how much of it to take.
constexpr uint32_t kMaxTrackedBytes = 1024;

// How many distinct constant buffers are followed at once.
constexpr size_t kMaxTrackedBuffers = 64;

// ---- render thread ---------------------------------------------------------
// `slotKind` is the `ContextSlot` of the call that bound it, so a candidate can
// say which shader stage reads it.
void OnConstantBufferBound (uint32_t slotKind, uint32_t startSlot, ID3D11Buffer* buffer);

// The D3D11.1 form, which also says WHERE in the buffer the binding starts.
//
// ⚠️ `byteOffset` IS WHAT MAKES A RING BUFFER READABLE AT ALL, so this is not a
// richer variant of the call above -- on this host it is the only one that ever
// fires. It remembers the window as pending, and the next `Unmap` of that buffer
// is what copies it: at bind time the memory is not mapped and reading it would
// be a read of an unmapped range.
//
// ⚠️ THE WINDOW IT CAPTURES IS ONE DRAW STALE, DELIBERATELY. A ring is written
// under `Map`, unmapped, and only then bound, so the offset for the bytes just
// written is not known until after the chance to read them has gone. What this
// captures is the offset bound BEFORE the current map -- which under
// `WRITE_NO_OVERWRITE` is still intact in the same allocation, and which holds
// the previous draw's constants. Every 3D draw in a frame shares one
// view-projection and stage 3 scores at rest, so one draw of staleness cannot
// change the verdict. It would matter to stage 4, which measures freshness, and
// stage 4 must not be built on this path without re-reading this paragraph.
void OnConstantBufferBoundWindow (uint32_t slotKind, uint32_t startSlot, ID3D11Buffer* buffer,
                                  uint32_t byteOffset);
// `windowOffset` says which window of a ring these bytes came from; it is part
// of the tracked entry's key, and 0 for a buffer that is its own window.
void OnConstantBufferWrite (ID3D11Resource* resource, uint32_t byteOffset, const void* bytes,
                            uint32_t byteCount, uint32_t windowOffset);

// Say which shader stage and register a captured window was bound to. Does
// nothing if that window is not currently tracked.
//
// ⚠️ IT LABELS, IT DOES NOT CLAIM. Archicad binds far more buffers than it
// updates, and letting a bind fill the table would push the handful that
// actually carry per-frame data out of it. Only a captured WRITE claims an
// entry. This is also the whole reason the capture half does not need to see
// the tracked table's internals -- see ConstantBufferCapture.hpp.
void LabelCapturedWindow (ID3D11Resource* resource, uint32_t windowOffset, uint32_t slotKind,
                          uint32_t bindSlot, uint64_t frameId);

// The resource's constant-buffer byte width, or 0 for anything that is not one.
//
// ⚠️ ONE COM CALL PER RESOURCE POINTER, EVER, AND THE ANSWER IS CACHED. Asking
// `GetDesc` on every Map would be a COM call per buffer per frame on Archicad's
// render thread, which is exactly the added frame cost PLAT-RE118 measured this
// path to be sensitive to. Same rule, same construction, as
// `PresentHook::RememberChainWindow`.
uint32_t ConstantBufferWidth (ID3D11Resource* resource);

// ---- the Map/Unmap pair ----------------------------------------------------
// ⚠️ THE BYTES ARE READ AT `Unmap`, NEVER AT `Map`. A `WRITE_DISCARD` map hands
// back a fresh allocation whose contents are undefined; the caller's data only
// exists once it has written it, which is at Unmap. So `OnMapped` remembers the
// pointer and `OnUnmapping` is where the copy happens -- and the memory is still
// valid there, because Unmap has not been forwarded yet.
//
// ⚠️ READING A MAPPED UPLOAD BUFFER IS SLOW. It is usually write-combined, where
// reads are uncached and cost far more than writes. That is the most expensive
// thing this discovery path does, and it is why `ContextHook` defaults the
// Map/Unmap slots to OFF.
//
// ⚠️ THESE LIVE HERE AND NOT IN THE HOOK BECAUSE THE RULES ARE THE
// CLASSIFIER'S. What counts as a capturable buffer, how large is too large, and
// what happens to the pointer between the two calls are all questions about
// finding a view matrix, not about detouring a vtable. The hook only says when
// the two calls happened.

// Returns the width being tracked for this map, or 0 if it is not being tracked.
uint32_t OnMapped (ID3D11Resource* resource, const void* mappedPointer);

// Copies the bytes if this resource was remembered by `OnMapped`, and returns
// how many. Must be called BEFORE the original Unmap.
uint32_t OnUnmapping (ID3D11Resource* resource);

// ---- main thread -----------------------------------------------------------

// Publish the ACAPI-derived camera to score against, and say whether the view
// moved since the last call. `viewport` is stage 2's captured rectangle -- the
// GPU's, never the window's -- and it is what turns an NDC difference into
// pixels.
void SetReference (const float eye[3], const float target[3], float viewConeDegreesHorizontal,
                   const renderstate::GpuViewport& viewport, bool cameraMoved);

// Is there a reference to score against yet?
bool HasReference ();

struct Candidate {
    uint64_t buffer = 0;         // the ID3D11Resource, as an integer
    uint32_t byteOffset = 0;     // ABSOLUTE offset in the buffer of these 64
                                 // bytes: the bound window's start plus the
                                 // offset within it. On a ring the window start
                                 // is the larger half of that by far.
    uint32_t windowOffset = 0;   // where the captured window begins

    // ⚠️ FOR A PRODUCT CANDIDATE, WHERE THE SECOND MATRIX CAME FROM. `byteOffset`
    // is the affine one (the view) and this is the projective one; the candidate
    // is their product, in that order. Zero and meaningless for variants 0..3.
    uint32_t pairedOffset = 0;
    uint32_t variant = 0;        // 0 = as stored, 1 = transposed, 2 = inverse,
                                 // 3 = inverse of the transpose; 4..7 the
                                 // PRODUCT of two captured blocks (see
                                 // `pairedOffset`); 8..9 a captured AFFINE block
                                 // against OUR OWN projection; 10..11 OUR OWN
                                 // view against a captured PROJECTIVE block.
                                 //
                                 // ⚠️ 8..11 ARE THE HALF-TESTS AND THEY ARE THE
                                 // MOST INFORMATIVE ROWS IN THE TABLE. A whole
                                 // view-projection that misses says only "wrong
                                 // somewhere". A hybrid that HITS says which
                                 // half of the transform we have found and, by
                                 // elimination, which half of our own reference
                                 // is wrong -- one run instead of three.
    uint32_t shaderStage = 0;    // the ContextSlot that bound it, or Count if unseen
    uint32_t bindSlot = 0;       // the b# register
    uint32_t byteWidth = 0;

    uint32_t writes = 0;             // captured writes to this buffer
    uint32_t changesWhileMoving = 0; // writes that CHANGED these 64 bytes, view moving
    uint32_t changesWhileStill = 0;  // ... view still. A view matrix has few of these

    double   maxPixelError = 0.0;
    double   meanPixelError = 0.0;
    bool     scored = false;

    float    matrix[16] = {};    // as stored, before the variant is applied
};

// Score every tracked region and write the best `max` candidates, lowest error
// first. Returns how many were written. Cheap enough for the camera tick at a
// low rate; it is not called per frame.
size_t Classify (Candidate* out, size_t max);

// The best candidate from the last `Classify`, or an unscored one.
Candidate Best ();

struct CandidateStats {
    bool     referenceValid = false;
    uint32_t buffersTracked = 0;
    uint32_t buffersDropped = 0;   // distinct buffers beyond kMaxTrackedBuffers
    uint64_t writesCaptured = 0;

    // ⚠️ WHY A MAP WAS NOT CAPTURED, WHICH IS THE ONLY THING THAT MATTERS WHEN
    // `writesCaptured` IS ZERO. The ninth live run (2026-09-13) recorded 90316
    // Map/Unmap pairs on Archicad's context and captured NOT ONE of them, and
    // the report could only say "nothing scored" and list two innocent reasons.
    // Every rejection in `OnMapped` is now counted and named, because "zero
    // captured" is a question and these four numbers are the answer to it.
    uint64_t mapsSeen = 0;             // Archicad maps offered to the classifier
    uint64_t mapsNotConstantBuffer = 0;// not BIND_CONSTANT_BUFFER at all
    uint64_t mapsTooLarge = 0;         // a constant buffer wider than kMaxTrackedBytes
    uint64_t mapsNoSlot = 0;           // the sixteen-entry map table was full
    uint32_t largestConstantBytes = 0; // the widest constant buffer ever offered
    bool     bufferCacheFull = false;  // the 256-entry GetDesc cache stopped asking

    // The ring path. `windowsPending` saturating means binds are arriving faster
    // than the unmaps that drain them, and the ones dropped are the oldest.
    uint64_t windowsCaptured = 0;
    uint64_t windowsDropped = 0;
    uint32_t largestBoundOffset = 0;

    // ⚠️ WHAT THE CAPTURED BYTES LOOK LIKE, WHICH IS THE ONLY USEFUL THING TO
    // SAY WHEN NOTHING MATCHES. Runs eleven and thirteen both ended on "NO
    // MATCH" at about 1163 px, and neither could distinguish the three readings
    // that verdict covers: the camera is not in these buffers at all; it is
    // there but not as a single 4x4; or it is there as a 4x4 and our reference
    // is the thing that is wrong. A count of how many captured 64-byte blocks
    // are even SHAPED like a projection separates the first from the other two,
    // and it costs a handful of comparisons on a scan that already runs.
    //
    // The convention is `MatrixMath.hpp`'s: row-vector, row-major, so a
    // projection has m[2][3] = +/-1 with m[3][3] = 0, and an affine transform
    // has a last column of (0, 0, 0, 1).
    // How many tracked entries actually held bytes when the scan ran. Run
    // fourteen examined 265 blocks where 64 full entries would give 1024, and
    // the report had no way to say whether the rest were empty or unreadable.
    uint32_t entriesWithData = 0;

    uint32_t blocksExamined = 0;
    uint32_t blocksFinite = 0;
    uint32_t blocksAffine = 0;      // world, view, or any rigid/scale transform
    uint32_t blocksProjective = 0;  // projection or view-projection
    uint32_t firstProjectiveOffset = 0;

    uint32_t regionsScored = 0;
    double   bestMaxPixelError = 0.0;
    uint64_t bestBuffer = 0;
    uint32_t bestByteOffset = 0;
    uint32_t bestVariant = 0;
};
CandidateStats GetCandidateStats ();

// ---- stage 4's raw material ------------------------------------------------
// One captured 64-byte block that is shaped like part of a camera, with the
// scene pass it was bound in. `SameFrameCamera` turns these into a pair; this
// file only says what was captured and when.
struct CameraBlock {
    float    m[16] = {};
    uint32_t windowOffset = 0;
    uint32_t byteOffset = 0;     // absolute, for logging only -- the ring moves
    uint32_t shaderStage = 0;    // the ContextSlot that bound it
    uint32_t bindSlot = 0;       // the b# register
    uint64_t scenePass = 0;
    bool     projective = false; // false means affine: a view or a world matrix
};

// Copy out every camera-shaped block from a scene pass no older than
// `maxAgePasses` behind `newestPass`. MAIN THREAD.
size_t SnapshotCameraBlocks (CameraBlock* out, size_t max, uint64_t newestPass,
                             uint32_t maxAgePasses);

// MAIN THREAD. Forget every tracked buffer. Called when the hook is installed.
void Reset ();

// ---- stage 0a, the cheap oracle --------------------------------------------
// One nav-log row per call carrying the best candidate's pixel error beside the
// ACAPI camera and the frame id.
//
// ⚠️ THIS ONE ROW ANSWERS BOTH QUESTIONS THE DISCOVERY STAGES ASK, which is why
// the handoff puts the full PLAT-RE115 scoreboard AFTER stage 4 rather than
// before stage 1. Does the captured matrix agree with the reference at rest --
// did we find the right buffer. Does it change before Archicad's draws rather
// than after -- is it the current frame. Neither needs generation plumbing and
// neither needs the report rewritten.
void LogOracleRow (uint64_t frameId);

}   // namespace viewmatrix
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv

#endif
