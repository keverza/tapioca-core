// ArchViz/Dxgi/ViewMatrixCandidates -- see the header. Every rule about method
// and convention is there; this is the mechanism.

#include "ArchViz/Dxgi/ViewMatrixCandidates.hpp"

#include "ArchViz/Dxgi/ConstantBufferCapture.hpp"
#include "ArchViz/Dxgi/SameFrameCamera.hpp"

#include "ArchViz/Dxgi/ContextHook.hpp"          // ContextSlot
#include "ArchViz/Dxgi/RenderStateCapture.hpp"   // GpuViewport
#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/NavLog.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace viewmatrix {

namespace {

// ---- what is tracked -------------------------------------------------------
// One entry per captured window, MOST RECENT WINS.
//
// ⚠️ THIS TABLE USED TO NEVER EVICT, AND ON A RING THAT WAS EXACTLY BACKWARDS.
// The rule was written for a handful of long-lived per-frame buffers, where
// "claimed on first write and never evicted" keeps the interesting ones and
// `buffersDropped` reports saturation honestly. Archicad 29 binds windows of an
// 8 MiB ring at a fresh cursor every frame, so run eleven captured 24314 windows
// into a 64-entry table, dropped 24240 of them, and scored the SIXTY-FOUR
// OLDEST -- windows from the first tenth of a second after arming, thirty
// seconds before the still view they were being compared against. Every
// candidate showed `changed 0 times while moving, 1 while still`, which is not a
// classification of a view matrix: it is the signature of an entry written once
// and never revisited.
//
// ⚠️ MOST-RECENT-WINS IS WHAT MAKES A RING SCOREABLE. Scoring happens at rest,
// Archicad stops redrawing when the view stops, so the newest captures are the
// last frame it drew -- drawn with the camera the reference is about to be read
// from. Sixty-four entries is then about a tenth of a second of history, which
// is the right window and not an arbitrary one.
//
// ⚠️ AND THE CHANGE COUNTERS STOP MEANING ANYTHING FOR RING WINDOWS. They
// compare an entry's previous bytes against the new ones, and after an eviction
// the previous bytes belonged to a different window of a different frame. They
// are reset on eviction and reported as unavailable rather than as zero, because
// a zero here reads as a finding.
struct Tracked {
    std::atomic<uint64_t> resource {0};
    // ⚠️ PART OF THE KEY, NOT A LABEL. One 8 MiB ring is bound at many different
    // offsets in a frame and each window is a different set of constants;
    // keying on the resource alone would give them all one entry, and every
    // capture would overwrite the last one's bytes under the same name.
    std::atomic<uint32_t> windowOffset {0};
    // ⚠️ ODD MEANS "BEING WRITTEN". The render thread bumps this to odd, copies,
    // then bumps it to even; a reader samples it before and after its own copy
    // and retries if either was odd or they differ. Without it a scored matrix
    // can be half of one upload and half of the next, which does not look like
    // corruption -- it looks like a candidate that nearly matches.
    std::atomic<uint32_t> sequence {0};
    std::atomic<uint32_t> byteWidth {0};
    std::atomic<uint32_t> writes {0};
    std::atomic<uint32_t> shaderStage {uint32_t (ContextSlot::Count)};
    std::atomic<uint32_t> bindSlot {0};
    // The frame the BIND that named this window happened in. See
    // ConstantBufferCapture.hpp's frame clock for why it is the bind's frame
    // and not the capture's.
    std::atomic<uint64_t> frameId {0};
    // Per 64-byte region, how many writes actually CHANGED it, split by whether
    // the view was moving at the time. This is the classifier the handoff asks
    // for and it is far cheaper than the projection score.
    std::atomic<uint32_t> changedMoving[kMaxTrackedBytes / 64] {};
    std::atomic<uint32_t> changedStill[kMaxTrackedBytes / 64] {};
    unsigned char bytes[kMaxTrackedBytes] = {};
};
Tracked g_tracked[kMaxTrackedBuffers];
std::atomic<uint32_t> g_trackedCursor {0};
// ⚠️ PLAIN, NOT ATOMIC, AND ONLY THE MAIN THREAD TOUCHES THEM. They are counted
// inside `Classify`, which runs on the camera tick and nowhere else; the render
// thread never sees them.
// ⚠️ CAPPED, BECAUSE THE PAIRING IS QUADRATIC. Thirty-two of each is 1024 pairs
// and four orderings apiece, which is four thousand scorings of eight points --
// nothing on a tick that only runs when the view is at rest, and a hard ceiling
// so a scene with hundreds of constant blocks cannot turn it into a stall.
constexpr size_t kMaxPairBlocks = 32;
struct PairBlock {
    float    m[16] = {};
    uint32_t offset = 0;        // absolute: window start plus the offset in it
    // ⚠️ THE ABSOLUTE OFFSET IS USELESS TO STAGE 4 AND THESE THREE ARE NOT.
    // Archicad's constants live in an 8 MiB ring whose cursor moves every frame,
    // so "the view matrix is at 3045376" is true for one frame and never again.
    // What is stable is the SHAPE of the binding: which shader stage, which b#
    // register, and how far into that register's window the matrix sits. That
    // triple is what a production hook watches for.
    uint32_t windowOffset = 0;
    uint32_t shaderStage = 0;
    uint32_t bindSlot = 0;
    uint64_t frameId = 0;
};
PairBlock g_affine[kMaxPairBlocks];
PairBlock g_projective[kMaxPairBlocks];
size_t    g_affineCount = 0;
size_t    g_projectiveCount = 0;

uint32_t g_entriesWithData = 0;
uint32_t g_blocksExamined = 0;
uint32_t g_blocksFinite = 0;
uint32_t g_blocksAffine = 0;
uint32_t g_blocksProjective = 0;
uint32_t g_firstProjectiveOffset = 0;
std::atomic<uint32_t> g_buffersTracked {0};
std::atomic<uint32_t> g_buffersDropped {0};
std::atomic<uint64_t> g_writesCaptured {0};

// Set by the main thread, read by the render thread on every captured write.
std::atomic<bool> g_cameraMoving {false};

// ---- the reference ---------------------------------------------------------
// Main thread only.
bool  g_referenceValid = false;
float g_referenceViewProj[16] = {};
float g_referenceView[16] = {};
float g_referenceProjection[16] = {};
float g_referenceEye[3] = {};
float g_referenceTarget[3] = {};
float g_referenceFovYDegrees = 0.0f;
float g_viewportWidth = 0.0f;
float g_viewportHeight = 0.0f;
// ⚠️ SCORING NEEDS A SETTLED VIEW, so the reference carries how long it has been
// still. One tick is not enough: the ACAPI read that produced it may itself be
// the last sample of a gesture, and the frame Archicad drew with may still be
// catching up.
uint32_t g_stillTicks = 0;
constexpr uint32_t kStillTicksBeforeScoring = 3;

Candidate g_best;
uint32_t  g_regionsScored = 0;

// ---- matrix helpers --------------------------------------------------------
// Row-vector, row-major, matching `MatrixMath` exactly -- see its header for why
// that convention is not negotiable in this tree.

void Transpose (float out[16], const float m[16])
{
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column)
            out[row * 4 + column] = m[column * 4 + row];
    }
}

// A general 4x4 inverse by cofactors. ⚠️ IT IS HERE RATHER THAN IN `MatrixMath`
// BECAUSE NOTHING ELSE IN THE RENDERER NEEDS ONE, and MatrixMath's contract is
// that it reproduces bx element for element -- adding a function bx does not
// have would blur what that file promises. If stage 6 needs an inverse on a
// rendering path, move it there WITH tests rather than calling across.
bool Invert (float out[16], const float m[16])
{
    float inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

    const float determinant = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (!(std::fabs (determinant) > 1e-20f) || !std::isfinite (determinant))
        return false;
    const float scale = 1.0f / determinant;
    for (int i = 0; i < 16; ++i) {
        out[i] = inv[i] * scale;
        if (!std::isfinite (out[i]))
            return false;
    }
    return true;
}

bool Finite (const float m[16])
{
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite (m[i]))
            return false;
    }
    return true;
}

// ---- the score -------------------------------------------------------------
// Project a spread of world points through both matrices and compare where they
// land, in pixels.
//
// ⚠️ THE POINTS ARE GENERATED FROM THE REFERENCE CAMERA'S OWN FRUSTUM, not from
// the model. Fixed world points would be off screen for most cameras, and a
// point behind the eye divides by a w near zero and produces an error of
// millions that says nothing about the matrix. Building them as eye + forward*d
// + right*u + up*v guarantees every one of them is in view of the REFERENCE, and
// a candidate that disagrees about where they land is then disagreeing about
// something visible.
constexpr int kDepthSamples = 2;
constexpr int kGridSamples = 3;
constexpr int kPointCount = kDepthSamples * kGridSamples * kGridSamples;

void BuildProbePoints (float points[kPointCount][3])
{
    float forward[3];
    float right[3];
    float up[3];
    CameraBasis (g_referenceEye, g_referenceTarget, forward, right, up);

    const float halfFov = g_referenceFovYDegrees * 0.5f * 3.14159265358979f / 180.0f;
    const float tangent = std::tan (halfFov);
    const float aspect = (g_viewportHeight > 0.0f) ? (g_viewportWidth / g_viewportHeight) : 1.0f;
    const float distances[kDepthSamples] = {5.0f, 60.0f};
    const float offsets[kGridSamples] = {-0.6f, 0.0f, 0.6f};

    int index = 0;
    for (int d = 0; d < kDepthSamples; ++d) {
        const float distance = distances[d];
        for (int v = 0; v < kGridSamples; ++v) {
            for (int h = 0; h < kGridSamples; ++h) {
                const float vertical = offsets[v] * distance * tangent;
                const float horizontal = offsets[h] * distance * tangent * aspect;
                for (int axis = 0; axis < 3; ++axis) {
                    points[index][axis] = g_referenceEye[axis] + forward[axis] * distance +
                                          right[axis] * horizontal + up[axis] * vertical;
                }
                ++index;
            }
        }
    }
}

// World point to pixel through `m`. False when the point is behind the eye or
// the divide is degenerate -- the caller drops the sample rather than scoring a
// number that means nothing.
bool ProjectToPixel (const float m[16], const float point[3], double& pixelX, double& pixelY)
{
    const float homogeneous[4] = {point[0], point[1], point[2], 1.0f};
    float clip[4] = {};
    TransformPoint (clip, homogeneous, m);
    if (!(std::fabs (clip[3]) > 1e-6f) || !std::isfinite (clip[3]))
        return false;
    const float ndcX = clip[0] / clip[3];
    const float ndcY = clip[1] / clip[3];
    if (!std::isfinite (ndcX) || !std::isfinite (ndcY))
        return false;
    // ⚠️ THE SAME MAPPING IS APPLIED TO BOTH MATRICES, so a flipped Y convention
    // cancels and does not show up as error. That is intentional: which way up
    // NDC runs is a rendering convention, not evidence about which buffer holds
    // the camera, and stage 3's acceptance is a screen-position agreement.
    pixelX = (double (ndcX) * 0.5 + 0.5) * double (g_viewportWidth);
    pixelY = (0.5 - double (ndcY) * 0.5) * double (g_viewportHeight);
    return true;
}

// Fills `maxError` and `meanError`. False when too few points projected for the
// answer to mean anything.
bool ScoreAgainstReference (const float candidate[16], double& maxError, double& meanError)
{
    if (!Finite (candidate))
        return false;

    float points[kPointCount][3];
    BuildProbePoints (points);

    maxError = 0.0;
    double total = 0.0;
    int scored = 0;
    for (int i = 0; i < kPointCount; ++i) {
        double referenceX = 0.0;
        double referenceY = 0.0;
        double candidateX = 0.0;
        double candidateY = 0.0;
        if (!ProjectToPixel (g_referenceViewProj, points[i], referenceX, referenceY))
            continue;
        if (!ProjectToPixel (candidate, points[i], candidateX, candidateY)) {
            // ⚠️ A POINT THE CANDIDATE CANNOT PROJECT IS A FAILED CANDIDATE, not
            // a skipped sample. The reference put it on screen; a matrix that
            // sends it behind the eye is not the same transform, and silently
            // dropping it would let a badly wrong matrix score well on the two
            // points it happened to survive.
            return false;
        }
        const double dx = candidateX - referenceX;
        const double dy = candidateY - referenceY;
        const double error = std::sqrt (dx * dx + dy * dy);
        maxError = std::max (maxError, error);
        total += error;
        ++scored;
    }
    if (scored < kPointCount / 2)
        return false;
    meanError = total / double (scored);
    return true;
}

// ---- the tracked table -----------------------------------------------------

bool Matches (const Tracked& entry, uint64_t key, uint32_t window)
{
    return entry.resource.load (std::memory_order_acquire) == key &&
           entry.windowOffset.load (std::memory_order_relaxed) == window;
}

Tracked* FindOrClaim (uint64_t key, uint32_t window)
{
    for (Tracked& entry : g_tracked) {
        const uint64_t seen = entry.resource.load (std::memory_order_acquire);
        if (seen == key && entry.windowOffset.load (std::memory_order_relaxed) == window)
            return &entry;
        if (seen != 0)
            continue;
        uint64_t expected = 0;
        if (entry.resource.compare_exchange_strong (expected, key, std::memory_order_acq_rel)) {
            // ⚠️ THE WINDOW IS STORED AFTER THE CAS AND THAT IS A REAL, NARROW
            // RACE: a concurrent lookup for the same buffer at a different
            // window can see this entry with a stale window for a few
            // instructions and claim a second entry for the same pair. The cost
            // is one duplicate row in a 64-entry table, which the classifier
            // scores twice and reports once. A lock here would be on Archicad's
            // render thread, which is not a trade worth making for that.
            entry.windowOffset.store (window, std::memory_order_relaxed);
            g_buffersTracked.fetch_add (1, std::memory_order_relaxed);
            return &entry;
        }
        if (Matches (entry, key, window))
            return &entry;
    }
    // ⚠️ NO FREE SLOT: TAKE THE OLDEST, ROUND ROBIN. The cursor sweeps the table,
    // so the entry reused is the one written longest ago.
    //
    // ⚠️ THE SEQUENCE COUNTER IS BUMPED ACROSS THE RE-KEY, exactly as a write is,
    // and the reader checks the key again afterwards. Without that a classifier
    // could copy the first half of one window's bytes and the second half of the
    // next window's, under whichever name it happened to read first -- and that
    // does not look like corruption, it looks like a candidate that nearly
    // matches. That failure mode is why the seqlock is here at all.
    const uint32_t at =
        g_trackedCursor.fetch_add (1, std::memory_order_relaxed) % uint32_t (kMaxTrackedBuffers);
    Tracked& entry = g_tracked[at];
    entry.sequence.fetch_add (1, std::memory_order_acq_rel);    // odd: being re-keyed
    entry.resource.store (key, std::memory_order_relaxed);
    entry.windowOffset.store (window, std::memory_order_relaxed);
    entry.byteWidth.store (0, std::memory_order_relaxed);
    entry.writes.store (0, std::memory_order_relaxed);
    for (std::atomic<uint32_t>& region : entry.changedMoving)
        region.store (0, std::memory_order_relaxed);
    for (std::atomic<uint32_t>& region : entry.changedStill)
        region.store (0, std::memory_order_relaxed);
    entry.sequence.fetch_add (1, std::memory_order_release);    // even: readable
    g_buffersDropped.fetch_add (1, std::memory_order_relaxed);
    return &entry;
}

Tracked* Find (uint64_t key, uint32_t window)
{
    for (Tracked& entry : g_tracked) {
        if (Matches (entry, key, window))
            return &entry;
    }
    return nullptr;
}

// Copy one entry's bytes out under the sequence counter. False if the render
// thread kept overwriting it -- which is itself a finding about how hot that
// buffer is, but not a scoreable sample.
bool ReadStable (const Tracked& entry, unsigned char* out, uint32_t bytes,
                 uint64_t expectedResource, uint32_t expectedWindow)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        // ⚠️ THE KEY IS PART OF WHAT IS BEING READ STABLY, not a precondition
        // checked once outside. An eviction between the caller's look and this
        // copy would hand back another window's bytes under this window's name.
        if (!Matches (entry, expectedResource, expectedWindow))
            return false;
        const uint32_t before = entry.sequence.load (std::memory_order_acquire);
        if ((before & 1u) != 0)
            continue;
        std::memcpy (out, entry.bytes, bytes);
        const uint32_t after = entry.sequence.load (std::memory_order_acquire);
        if (before == after)
            return true;
    }
    return false;
}


}   // namespace


void LabelCapturedWindow (ID3D11Resource* resource, uint32_t windowOffset, uint32_t slotKind,
                          uint32_t bindSlot, uint64_t frameId)
{
    if (resource == nullptr)
        return;
    Tracked* entry = Find (uint64_t (uintptr_t (resource)), windowOffset);
    if (entry == nullptr)
        return;
    entry->shaderStage.store (slotKind, std::memory_order_relaxed);
    entry->bindSlot.store (bindSlot, std::memory_order_relaxed);
    entry->frameId.store (frameId, std::memory_order_relaxed);
}

void OnConstantBufferWrite (ID3D11Resource* resource, uint32_t byteOffset, const void* bytes,
                            uint32_t byteCount, uint32_t windowOffset)
{
    if (resource == nullptr || bytes == nullptr || byteCount == 0)
        return;
    if (byteOffset >= kMaxTrackedBytes)
        return;
    const uint32_t copyBytes = std::min (byteCount, kMaxTrackedBytes - byteOffset);

    Tracked* entry = FindOrClaim (uint64_t (uintptr_t (resource)), windowOffset);
    if (entry == nullptr)
        return;

    const bool moving = g_cameraMoving.load (std::memory_order_relaxed);

    // Which 64-byte regions this write actually CHANGED, decided before the copy
    // because afterwards there is nothing left to compare against.
    const uint32_t firstRegion = byteOffset / 64;
    const uint32_t lastRegion = (byteOffset + copyBytes - 1) / 64;
    const unsigned char* source = static_cast<const unsigned char*> (bytes);
    for (uint32_t region = firstRegion; region <= lastRegion &&
                                        region < kMaxTrackedBytes / 64; ++region) {
        const uint32_t regionStart = std::max (region * 64u, byteOffset);
        const uint32_t regionEnd = std::min ((region + 1) * 64u, byteOffset + copyBytes);
        if (regionEnd <= regionStart)
            continue;
        const bool changed = std::memcmp (entry->bytes + regionStart,
                                          source + (regionStart - byteOffset),
                                          regionEnd - regionStart) != 0;
        if (!changed)
            continue;
        if (moving)
            entry->changedMoving[region].fetch_add (1, std::memory_order_relaxed);
        else
            entry->changedStill[region].fetch_add (1, std::memory_order_relaxed);
    }

    entry->sequence.fetch_add (1, std::memory_order_acq_rel);   // odd: writing
    std::memcpy (entry->bytes + byteOffset, source, copyBytes);
    entry->sequence.fetch_add (1, std::memory_order_release);   // even: readable

    const uint32_t width = byteOffset + copyBytes;
    if (width > entry->byteWidth.load (std::memory_order_relaxed))
        entry->byteWidth.store (width, std::memory_order_relaxed);
    entry->writes.fetch_add (1, std::memory_order_relaxed);
    g_writesCaptured.fetch_add (1, std::memory_order_relaxed);
}

void SetReference (const float eye[3], const float target[3], float viewConeDegreesHorizontal,
                   const renderstate::GpuViewport& viewport, bool cameraMoved)
{
    g_cameraMoving.store (cameraMoved, std::memory_order_relaxed);
    g_stillTicks = cameraMoved ? 0 : (g_stillTicks + 1);

    g_viewportWidth = viewport.width;
    g_viewportHeight = viewport.height;
    if (!(g_viewportWidth > 1.0f) || !(g_viewportHeight > 1.0f)) {
        g_referenceValid = false;
        return;
    }

    // ⚠️ THE CONE IS HORIZONTAL AND THE PROJECTION WANTS VERTICAL. Archicad's
    // `viewCone` is a horizontal field of view in degrees -- confirmed in the
    // projection-overlay work and repeated in `MatrixMath`'s own header -- and
    // converting one of the pair rather than both is exactly how a systematic
    // error gets built into a score that then looks like a near miss.
    const float aspect = g_viewportWidth / g_viewportHeight;
    const float halfHorizontal = viewConeDegreesHorizontal * 0.5f * 3.14159265358979f / 180.0f;
    const float halfVertical = std::atan (std::tan (halfHorizontal) / std::max (aspect, 1e-3f));
    g_referenceFovYDegrees = halfVertical * 2.0f * 180.0f / 3.14159265358979f;

    for (int i = 0; i < 3; ++i) {
        g_referenceEye[i] = eye[i];
        g_referenceTarget[i] = target[i];
    }

    // ⚠️ THE NEAR AND FAR PLANES ARE OURS, NOT ARCHICAD'S, and that is fine
    // BECAUSE THE SCORE IGNORES DEPTH. They change clip z and the w divide not
    // at all -- w is -z_view under `PerspectiveRH` regardless of the planes -- so
    // the pixel positions this file compares are unaffected. If a later stage
    // starts scoring depth, these stop being free and have to be recovered from
    // the captured matrix instead of chosen here.
    const float up[3] = {0.0f, 0.0f, 1.0f};
    float view[16];
    float projection[16];
    LookAtRH (view, g_referenceEye, g_referenceTarget, up);
    PerspectiveRH (projection, g_referenceFovYDegrees, aspect, 0.1f, 10000.0f);
    Multiply (g_referenceViewProj, view, projection);
    // ⚠️ THE HALVES ARE KEPT SEPARATELY SO THE CLASSIFIER CAN TEST THEM ONE AT A
    // TIME. Scoring a captured view against OUR projection, and OUR view against
    // a captured projection, turns "the whole transform is wrong" into "this
    // half is right and that half is not" -- which is the difference between
    // knowing what to fix and running the test again.
    std::memcpy (g_referenceView, view, sizeof (g_referenceView));
    std::memcpy (g_referenceProjection, projection, sizeof (g_referenceProjection));
    g_referenceValid = Finite (g_referenceViewProj);
}

bool HasReference ()
{
    return g_referenceValid;
}

// ⚠️ SHAPE, NOT VALUE. This says nothing about whether a block is the RIGHT
// matrix -- only whether it is the right kind of object. See `CandidateStats`
// for why that distinction is what a "no match" run needs.
//
// The tolerances are loose on purpose. A projection's w column is exactly +/-1
// and 0 in the maths, but these bytes have been through a float pipeline and a
// perspective matrix built for a reversed or infinite far plane puts small
// non-zero values where the textbook puts exact ones. Loose enough to catch
// those, tight enough that arbitrary material constants do not qualify.
void Census (const float* m, uint32_t absoluteOffset, uint32_t windowOffset,
             uint32_t shaderStage, uint32_t bindSlot, uint64_t frameId)
{
    ++g_blocksExamined;
    if (!Finite (m))
        return;
    ++g_blocksFinite;
    // ⚠️ THE BLOCKS ARE KEPT, NOT JUST COUNTED, and that is what run fourteen
    // bought. Its census found 38 affine blocks and 14 projective ones in the
    // captured windows while every single-block candidate missed by 1163 px --
    // which is exactly the picture you get when the renderer uploads the VIEW
    // and the PROJECTION as separate constants and their product never exists in
    // memory for anyone to find. Keeping them lets the classifier multiply them
    // together and score that instead.

    const float wx = m[3], wy = m[7], wz = m[11], ww = m[15];
    const bool axisAligned = std::fabs (wx) < 1e-4f && std::fabs (wy) < 1e-4f;

    if (axisAligned && std::fabs (wz) < 1e-4f && std::fabs (ww - 1.0f) < 1e-3f) {
        ++g_blocksAffine;
        if (g_affineCount < kMaxPairBlocks) {
            PairBlock& block = g_affine[g_affineCount];
            std::memcpy (block.m, m, sizeof (block.m));
            block.offset = absoluteOffset;
            block.windowOffset = windowOffset;
            block.shaderStage = shaderStage;
            block.bindSlot = bindSlot;
            block.frameId = frameId;
            ++g_affineCount;
        }
        return;
    }
    if (axisAligned && std::fabs (std::fabs (wz) - 1.0f) < 1e-2f &&
        std::fabs (ww) < 1e-2f) {
        // The scale terms have to be there too, or a matrix of mostly zeroes
        // with a stray 1 in the w column would qualify.
        if (std::fabs (m[0]) > 1e-6f && std::fabs (m[5]) > 1e-6f) {
            if (g_blocksProjective == 0)
                g_firstProjectiveOffset = absoluteOffset;
            ++g_blocksProjective;
            if (g_projectiveCount < kMaxPairBlocks) {
                PairBlock& block = g_projective[g_projectiveCount];
                std::memcpy (block.m, m, sizeof (block.m));
                block.offset = absoluteOffset;
                block.windowOffset = windowOffset;
                block.shaderStage = shaderStage;
                block.bindSlot = bindSlot;
                block.frameId = frameId;
                ++g_projectiveCount;
            }
        }
    }
}

// Keep the best sixteen by max pixel error. Shared by the single-block scan and
// the pair scan so the two cannot disagree about ordering.
void Insert (Candidate* found, size_t& foundCount, const Candidate& result)
{
    size_t position = foundCount;
    while (position > 0 && found[position - 1].maxPixelError > result.maxPixelError) {
        if (position < 16)
            found[position] = found[position - 1];
        --position;
    }
    if (position < 16) {
        found[position] = result;
        foundCount = (foundCount < 16) ? (foundCount + 1) : 16;
    }
}

size_t Classify (Candidate* out, size_t max)
{
    g_regionsScored = 0;
    g_blocksExamined = 0;
    g_blocksFinite = 0;
    g_blocksAffine = 0;
    g_blocksProjective = 0;
    g_firstProjectiveOffset = 0;
    g_affineCount = 0;
    g_projectiveCount = 0;
    g_entriesWithData = 0;
    if (out == nullptr || max == 0)
        return 0;
    // Scoring a moving view would penalise a candidate for being fresher than
    // the reference -- see the header.
    if (!g_referenceValid || g_stillTicks < kStillTicksBeforeScoring)
        return 0;

    Candidate found[16];
    size_t foundCount = 0;

    unsigned char snapshot[kMaxTrackedBytes];
    for (const Tracked& entry : g_tracked) {
        const uint64_t resource = entry.resource.load (std::memory_order_acquire);
        if (resource == 0)
            continue;
        const uint32_t window = entry.windowOffset.load (std::memory_order_relaxed);
        const uint32_t width = entry.byteWidth.load (std::memory_order_relaxed);
        if (width < 64)
            continue;
        if (!ReadStable (entry, snapshot, std::min (width, kMaxTrackedBytes), resource, window))
            continue;
        ++g_entriesWithData;

        for (uint32_t offset = 0; offset + 64 <= width; offset += 64) {
            float stored[16];
            std::memcpy (stored, snapshot + offset, sizeof (stored));
            Census (stored, window + offset, window,
                    entry.shaderStage.load (std::memory_order_relaxed),
                    entry.bindSlot.load (std::memory_order_relaxed),
                    entry.frameId.load (std::memory_order_relaxed));
            if (!Finite (stored))
                continue;

            for (uint32_t variant = 0; variant < 4; ++variant) {
                float candidate[16];
                switch (variant) {
                    case 0:
                        std::memcpy (candidate, stored, sizeof (candidate));
                        break;
                    case 1:
                        Transpose (candidate, stored);
                        break;
                    case 2:
                        if (!Invert (candidate, stored))
                            continue;
                        break;
                    default: {
                        float transposed[16];
                        Transpose (transposed, stored);
                        if (!Invert (candidate, transposed))
                            continue;
                        break;
                    }
                }

                double maxError = 0.0;
                double meanError = 0.0;
                ++g_regionsScored;
                if (!ScoreAgainstReference (candidate, maxError, meanError))
                    continue;

                Candidate result;
                result.buffer = resource;
                // ⚠️ ABSOLUTE, so the number can be compared with a bind's
                // `firstConstant * 16`. Within a ring, "offset 64" is meaningless
                // on its own -- there are eight thousand of them.
                result.windowOffset = window;
                result.byteOffset = window + offset;
                result.variant = variant;
                result.shaderStage = entry.shaderStage.load (std::memory_order_relaxed);
                result.bindSlot = entry.bindSlot.load (std::memory_order_relaxed);
                result.byteWidth = width;
                result.writes = entry.writes.load (std::memory_order_relaxed);
                const uint32_t region = offset / 64;
                if (region < kMaxTrackedBytes / 64) {
                    result.changesWhileMoving =
                        entry.changedMoving[region].load (std::memory_order_relaxed);
                    result.changesWhileStill =
                        entry.changedStill[region].load (std::memory_order_relaxed);
                }
                result.maxPixelError = maxError;
                result.meanPixelError = meanError;
                result.scored = true;
                std::memcpy (result.matrix, stored, sizeof (result.matrix));

                // Insertion into a small best-of table. Keeping every scored
                // region would be thousands of rows a tick for no gain: what the
                // stage needs is the handful that came close.
                Insert (found, foundCount, result);
            }
        }
    }

    // ---- products of pairs -------------------------------------------------
    // ⚠️ THIS IS WHERE A RENDERER THAT UPLOADS VIEW AND PROJECTION SEPARATELY
    // GETS FOUND, AND NOTHING ABOVE COULD EVER FIND IT. Every candidate so far
    // is one captured 64-byte block; if Archicad hands the shaders a view matrix
    // in one constant and a projection in another and lets the GPU combine them,
    // then the view-projection this stage is looking for HAS NEVER EXISTED IN
    // MEMORY and no amount of scanning bytes will produce it. Run fourteen made
    // that the live hypothesis: 38 affine blocks and 14 projective ones in the
    // captured windows, and every single-block candidate missing by 1163 px --
    // not a near miss, a different transform.
    //
    // ⚠️ FOUR ORDERINGS PER PAIR, BECAUSE HLSL PACKS `float4x4` COLUMN-MAJOR.
    // `MatrixMath` is row-vector row-major, so a matrix uploaded for a shader
    // arrives transposed relative to this file's convention -- and it is the
    // TRANSPOSED pair whose product is the expected hit, not the raw one. Trying
    // both of each is four combinations and removes the guess.
    for (size_t a = 0; a < g_affineCount; ++a) {
        for (size_t b = 0; b < g_projectiveCount; ++b) {
            for (uint32_t variant = 0; variant < 4; ++variant) {
                float view[16];
                float projection[16];
                if ((variant & 1u) != 0)
                    Transpose (view, g_affine[a].m);
                else
                    std::memcpy (view, g_affine[a].m, sizeof (view));
                if ((variant & 2u) != 0)
                    Transpose (projection, g_projective[b].m);
                else
                    std::memcpy (projection, g_projective[b].m, sizeof (projection));

                float candidate[16];
                Multiply (candidate, view, projection);
                if (!Finite (candidate))
                    continue;

                double maxError = 0.0;
                double meanError = 0.0;
                ++g_regionsScored;
                if (!ScoreAgainstReference (candidate, maxError, meanError))
                    continue;

                Candidate result;
                result.byteOffset = g_affine[a].offset;
                result.windowOffset = g_affine[a].windowOffset;
                result.shaderStage = g_affine[a].shaderStage;
                result.bindSlot = g_affine[a].bindSlot;
                result.pairedOffset = g_projective[b].offset;
                result.variant = 4u + variant;
                result.byteWidth = 64;
                result.maxPixelError = maxError;
                result.meanPixelError = meanError;
                result.scored = true;
                std::memcpy (result.matrix, candidate, sizeof (result.matrix));
                Insert (found, foundCount, result);
            }
        }
    }

    // ---- the half-tests ----------------------------------------------------
    // ⚠️ THESE ANSWER A DIFFERENT QUESTION FROM EVERYTHING ABOVE, and it is the
    // question a run of "no match" leaves behind. Above, a candidate is a whole
    // view-projection and a miss says only that something is wrong. Here each
    // captured half is scored against OUR OWN other half: if a captured affine
    // block times our projection lands on the reference, we have found
    // Archicad's VIEW and our projection is close enough to see it -- and by
    // elimination our own view construction is what was wrong. If our view times
    // a captured projective block lands, the reverse. Either outcome names the
    // half to fix. Both missing says the pieces are not view and projection at
    // all, which is also worth one run.
    for (size_t a = 0; a < g_affineCount; ++a) {
        for (uint32_t variant = 0; variant < 2; ++variant) {
            float view[16];
            if (variant != 0)
                Transpose (view, g_affine[a].m);
            else
                std::memcpy (view, g_affine[a].m, sizeof (view));

            float candidate[16];
            Multiply (candidate, view, g_referenceProjection);
            if (!Finite (candidate))
                continue;

            double maxError = 0.0;
            double meanError = 0.0;
            ++g_regionsScored;
            if (!ScoreAgainstReference (candidate, maxError, meanError))
                continue;

            Candidate result;
            result.byteOffset = g_affine[a].offset;
            result.windowOffset = g_affine[a].windowOffset;
            result.shaderStage = g_affine[a].shaderStage;
            result.bindSlot = g_affine[a].bindSlot;
            result.variant = 8u + variant;
            result.byteWidth = 64;
            result.maxPixelError = maxError;
            result.meanPixelError = meanError;
            result.scored = true;
            std::memcpy (result.matrix, candidate, sizeof (result.matrix));
            Insert (found, foundCount, result);
        }
    }

    for (size_t b = 0; b < g_projectiveCount; ++b) {
        for (uint32_t variant = 0; variant < 2; ++variant) {
            float projection[16];
            if (variant != 0)
                Transpose (projection, g_projective[b].m);
            else
                std::memcpy (projection, g_projective[b].m, sizeof (projection));

            float candidate[16];
            Multiply (candidate, g_referenceView, projection);
            if (!Finite (candidate))
                continue;

            double maxError = 0.0;
            double meanError = 0.0;
            ++g_regionsScored;
            if (!ScoreAgainstReference (candidate, maxError, meanError))
                continue;

            Candidate result;
            result.byteOffset = g_projective[b].offset;
            result.windowOffset = g_projective[b].windowOffset;
            result.shaderStage = g_projective[b].shaderStage;
            result.bindSlot = g_projective[b].bindSlot;
            result.variant = 10u + variant;
            result.byteWidth = 64;
            result.maxPixelError = maxError;
            result.meanPixelError = meanError;
            result.scored = true;
            std::memcpy (result.matrix, candidate, sizeof (result.matrix));
            Insert (found, foundCount, result);
        }
    }

    const size_t written = std::min (foundCount, max);
    for (size_t i = 0; i < written; ++i)
        out[i] = found[i];
    g_best = (foundCount > 0) ? found[0] : Candidate {};
    return written;
}

size_t SnapshotCameraBlocks (CameraBlock* out, size_t max, uint64_t newestPass,
                             uint32_t maxAgePasses)
{
    if (out == nullptr || max == 0 || newestPass == 0)
        return 0;

    // ⚠️ ONE WALK, TAKEN FRESH, AND NOT CACHED. The tracked table is
    // most-recent-wins and the render thread rewrites its entries while this
    // runs, so an index kept between calls would name a different window by the
    // time it was used. Sixty-four entries of sixteen blocks is a few thousand
    // comparisons -- less than the ACAPI read the same tick already makes.
    size_t written = 0;
    unsigned char snapshot[kMaxTrackedBytes];
    for (const Tracked& entry : g_tracked) {
        if (written >= max)
            break;
        const uint64_t resource = entry.resource.load (std::memory_order_acquire);
        if (resource == 0)
            continue;
        const uint32_t window = entry.windowOffset.load (std::memory_order_relaxed);
        const uint32_t width = entry.byteWidth.load (std::memory_order_relaxed);
        if (width < 64)
            continue;
        const uint64_t pass = entry.frameId.load (std::memory_order_relaxed);
        if (pass == 0 || pass > newestPass || (newestPass - pass) > maxAgePasses)
            continue;
        if (!ReadStable (entry, snapshot, std::min (width, kMaxTrackedBytes), resource, window))
            continue;

        const uint32_t stage = entry.shaderStage.load (std::memory_order_relaxed);
        const uint32_t slot = entry.bindSlot.load (std::memory_order_relaxed);
        for (uint32_t offset = 0; offset + 64 <= width && written < max; offset += 64) {
            float stored[16];
            std::memcpy (stored, snapshot + offset, sizeof (stored));
            if (!Finite (stored))
                continue;

            // Same shape test as the census, and deliberately the same
            // tolerances -- see `Census`. A block that is neither is somebody
            // else's constants and is not offered.
            const float wx = stored[3], wy = stored[7], wz = stored[11], ww = stored[15];
            const bool axisAligned = std::fabs (wx) < 1e-4f && std::fabs (wy) < 1e-4f;
            const bool affine = axisAligned && std::fabs (wz) < 1e-4f &&
                                std::fabs (ww - 1.0f) < 1e-3f;
            const bool projective = axisAligned && std::fabs (std::fabs (wz) - 1.0f) < 1e-2f &&
                                    std::fabs (ww) < 1e-2f && std::fabs (stored[0]) > 1e-6f &&
                                    std::fabs (stored[5]) > 1e-6f;
            if (!affine && !projective)
                continue;

            CameraBlock& block = out[written++];
            std::memcpy (block.m, stored, sizeof (block.m));
            block.windowOffset = window;
            block.byteOffset = window + offset;
            block.shaderStage = stage;
            block.bindSlot = slot;
            block.scenePass = pass;
            block.projective = projective;
        }
    }
    return written;
}

Candidate Best ()
{
    return g_best;
}

CandidateStats GetCandidateStats ()
{
    CandidateStats stats;
    stats.referenceValid = g_referenceValid;
    stats.buffersTracked = g_buffersTracked.load (std::memory_order_relaxed);
    FillCaptureStats (stats);
    stats.entriesWithData = g_entriesWithData;
    stats.blocksExamined = g_blocksExamined;
    stats.blocksFinite = g_blocksFinite;
    stats.blocksAffine = g_blocksAffine;
    stats.blocksProjective = g_blocksProjective;
    stats.firstProjectiveOffset = g_firstProjectiveOffset;
    stats.buffersDropped = g_buffersDropped.load (std::memory_order_relaxed);
    stats.writesCaptured = g_writesCaptured.load (std::memory_order_relaxed);
    stats.regionsScored = g_regionsScored;
    if (g_best.scored) {
        stats.bestMaxPixelError = g_best.maxPixelError;
        stats.bestBuffer = g_best.buffer;
        stats.bestByteOffset = g_best.byteOffset;
        stats.bestVariant = g_best.variant;
    }
    return stats;
}

void Reset ()
{
    for (Tracked& entry : g_tracked) {
        entry.resource.store (0, std::memory_order_relaxed);
        entry.sequence.store (0, std::memory_order_relaxed);
        entry.byteWidth.store (0, std::memory_order_relaxed);
        entry.writes.store (0, std::memory_order_relaxed);
        entry.shaderStage.store (uint32_t (ContextSlot::Count), std::memory_order_relaxed);
        entry.bindSlot.store (0, std::memory_order_relaxed);
        for (uint32_t region = 0; region < kMaxTrackedBytes / 64; ++region) {
            entry.changedMoving[region].store (0, std::memory_order_relaxed);
            entry.changedStill[region].store (0, std::memory_order_relaxed);
        }
        std::memset (entry.bytes, 0, sizeof (entry.bytes));
    }
    g_buffersTracked.store (0, std::memory_order_relaxed);
    for (Tracked& entry : g_tracked)
        entry.windowOffset.store (0, std::memory_order_relaxed);
    g_trackedCursor.store (0, std::memory_order_relaxed);
    ResetPairing ();

    // The capture half owns its own tables.
    ResetCapture ();
    g_buffersDropped.store (0, std::memory_order_relaxed);
    g_writesCaptured.store (0, std::memory_order_relaxed);
    g_best = Candidate {};
    g_regionsScored = 0;
    g_stillTicks = 0;
    g_referenceValid = false;
}

void LogOracleRow (uint64_t frameId)
{
    if (!navlog::IsRunning ())
        return;
    const Candidate best = Best ();
    navlog::LogGpuOracle (frameId, g_referenceEye, g_referenceTarget, g_referenceFovYDegrees,
                          g_viewportWidth, g_viewportHeight, best.scored, best.buffer,
                          best.byteOffset, best.variant, best.maxPixelError,
                          best.meanPixelError, best.changesWhileMoving, best.changesWhileStill);
}

}   // namespace viewmatrix
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv
