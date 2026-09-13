// ArchViz/Dxgi/ViewMatrixCandidates -- see the header. Every rule about method
// and convention is there; this is the mechanism.

#include "ArchViz/Dxgi/ViewMatrixCandidates.hpp"

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
// One entry per distinct constant buffer, claimed on first write and never
// evicted. ⚠️ NO EVICTION IS DELIBERATE, for the reason `ContextHook`'s buffer
// cache gives: an eviction policy on a lock-free table read from a render thread
// is a source of races bought for nothing, and `buffersDropped` says plainly
// that the table saturated instead of the data quietly thinning.
struct Tracked {
    std::atomic<uint64_t> resource {0};
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
    // Per 64-byte region, how many writes actually CHANGED it, split by whether
    // the view was moving at the time. This is the classifier the handoff asks
    // for and it is far cheaper than the projection score.
    std::atomic<uint32_t> changedMoving[kMaxTrackedBytes / 64] {};
    std::atomic<uint32_t> changedStill[kMaxTrackedBytes / 64] {};
    unsigned char bytes[kMaxTrackedBytes] = {};
};
Tracked g_tracked[kMaxTrackedBuffers];
std::atomic<uint32_t> g_buffersTracked {0};
std::atomic<uint64_t> g_mapsSeen {0};
std::atomic<uint64_t> g_mapsNotConstantBuffer {0};
std::atomic<uint64_t> g_mapsTooLarge {0};
std::atomic<uint64_t> g_mapsNoSlot {0};
std::atomic<uint32_t> g_largestConstantBytes {0};
std::atomic<uint32_t> g_buffersDropped {0};
std::atomic<uint64_t> g_writesCaptured {0};

// Set by the main thread, read by the render thread on every captured write.
std::atomic<bool> g_cameraMoving {false};

// ---- the reference ---------------------------------------------------------
// Main thread only.
bool  g_referenceValid = false;
float g_referenceViewProj[16] = {};
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

Tracked* FindOrClaim (uint64_t key)
{
    for (Tracked& entry : g_tracked) {
        const uint64_t seen = entry.resource.load (std::memory_order_acquire);
        if (seen == key)
            return &entry;
        if (seen != 0)
            continue;
        uint64_t expected = 0;
        if (entry.resource.compare_exchange_strong (expected, key, std::memory_order_acq_rel)) {
            g_buffersTracked.fetch_add (1, std::memory_order_relaxed);
            return &entry;
        }
        if (entry.resource.load (std::memory_order_acquire) == key)
            return &entry;
    }
    g_buffersDropped.fetch_add (1, std::memory_order_relaxed);
    return nullptr;
}

Tracked* Find (uint64_t key)
{
    for (Tracked& entry : g_tracked) {
        if (entry.resource.load (std::memory_order_acquire) == key)
            return &entry;
    }
    return nullptr;
}

// Copy one entry's bytes out under the sequence counter. False if the render
// thread kept overwriting it -- which is itself a finding about how hot that
// buffer is, but not a scoreable sample.
bool ReadStable (const Tracked& entry, unsigned char* out, uint32_t bytes)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
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

// ---- sizing a constant buffer ----------------------------------------------
// Moved here from `ContextHook` (2026-09-13) when that file reached the size
// cap, and it belongs here on the merits: what counts as a capturable buffer is
// a question about finding a view matrix. See the header for the caching rule.
constexpr size_t kBufferCacheSize = 256;
struct BufferInfo {
    std::atomic<uint64_t> resource {0};
    std::atomic<uint32_t> byteWidth {0};
    std::atomic<uint32_t> isConstantBuffer {0};
};
BufferInfo g_bufferCache[kBufferCacheSize];
std::atomic<bool> g_bufferCacheFull {false};

// ---- what is mapped right now ----------------------------------------------
constexpr size_t kMapTableSize = 16;
struct MapEntry {
    std::atomic<uint64_t> resource {0};
    std::atomic<uint64_t> pointer {0};
    std::atomic<uint32_t> bytes {0};
};
MapEntry g_mapped[kMapTableSize];

}   // namespace

uint32_t ConstantBufferWidth (ID3D11Resource* resource)
{
    const uint64_t key = uint64_t (uintptr_t (resource));
    if (key == 0)
        return 0;

    for (BufferInfo& entry : g_bufferCache) {
        const uint64_t seen = entry.resource.load (std::memory_order_acquire);
        if (seen == key) {
            return entry.isConstantBuffer.load (std::memory_order_relaxed)
                ? entry.byteWidth.load (std::memory_order_relaxed) : 0;
        }
        if (seen != 0)
            continue;

        uint32_t width = 0;
        uint32_t isConstant = 0;
        ID3D11Buffer* buffer = nullptr;
        if (SUCCEEDED (resource->QueryInterface (__uuidof (ID3D11Buffer), (void**) &buffer)) &&
            buffer != nullptr) {
            D3D11_BUFFER_DESC desc = {};
            buffer->GetDesc (&desc);
            if ((desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0) {
                isConstant = 1;
                width = desc.ByteWidth;
            }
            buffer->Release ();
        }
        // ⚠️ THE PAYLOAD IS PUBLISHED BEFORE THE KEY, for the reason
        // `RememberChainWindow` gives: a reader that saw the key first could
        // read a zero width and cache "this buffer is empty" for the session.
        entry.byteWidth.store (width, std::memory_order_relaxed);
        entry.isConstantBuffer.store (isConstant, std::memory_order_relaxed);
        entry.resource.store (key, std::memory_order_release);
        return isConstant ? width : 0;
    }

    // ⚠️ WHEN THE CACHE IS FULL IT STOPS ASKING RATHER THAN EVICTING. An eviction
    // policy on a lock-free table read from a render thread is a source of races
    // for no benefit: a scene with more distinct buffers than this has bigger
    // problems than a few uncached ones.
    g_bufferCacheFull.store (true, std::memory_order_relaxed);
    return 0;
}

uint32_t OnMapped (ID3D11Resource* resource, const void* mappedPointer)
{
    if (resource == nullptr || mappedPointer == nullptr)
        return 0;

    // ⚠️ COUNTED BEFORE ANYTHING ELSE, AND EVERY REJECTION IS NAMED. All four
    // of these are relaxed adds on already-hot cache lines, which is a price
    // worth paying once: a run that captures nothing has to be able to say
    // WHICH of "Archicad does not Map its constant buffers", "its constant
    // buffers are bigger than we are willing to copy" and "we ran out of table"
    // is true, and without these the report can only shrug.
    g_mapsSeen.fetch_add (1, std::memory_order_relaxed);

    const uint32_t width = ConstantBufferWidth (resource);
    if (width == 0) {
        g_mapsNotConstantBuffer.fetch_add (1, std::memory_order_relaxed);
        return 0;
    }

    // ⚠️ THE WIDEST ONE IS REMEMBERED EVEN THOUGH IT IS REFUSED, because the
    // number decides the next move. A few kilobytes means raise the cap; a
    // megabyte means Archicad packs its constants into one big ring and the
    // capture has to follow `firstConstant` into it instead of copying from the
    // start -- two different pieces of work, and this is what tells them apart.
    uint32_t seen = g_largestConstantBytes.load (std::memory_order_relaxed);
    while (width > seen &&
           !g_largestConstantBytes.compare_exchange_weak (seen, width,
                   std::memory_order_relaxed))
        ;

    if (width > kMaxTrackedBytes) {
        g_mapsTooLarge.fetch_add (1, std::memory_order_relaxed);
        return 0;
    }

    const uint64_t key = uint64_t (uintptr_t (resource));
    for (MapEntry& entry : g_mapped) {
        uint64_t expected = 0;
        // Claimed by CAS rather than by a store. The immediate context is single
        // threaded by D3D11's own contract, but Archicad may drive a deferred one
        // on another thread through the same vtable, and a torn claim here would
        // copy one buffer's bytes under another buffer's name.
        if (entry.resource.compare_exchange_strong (expected, key, std::memory_order_acq_rel)) {
            entry.pointer.store (uint64_t (uintptr_t (mappedPointer)), std::memory_order_relaxed);
            entry.bytes.store (width, std::memory_order_release);
            return width;
        }
    }
    // ⚠️ TABLE FULL. This used to return silently, on the reasoning that sixteen
    // simultaneous constant-buffer maps would mean D3D11's own threading
    // contract had been broken. That reasoning is sound and the silence was
    // still wrong: it made one of the three ways to capture nothing invisible,
    // and this rung has already lost six runs to an instrument that failed
    // quietly. A non-zero count here is a real finding, not a tuning knob.
    g_mapsNoSlot.fetch_add (1, std::memory_order_relaxed);
    return 0;
}

uint32_t OnUnmapping (ID3D11Resource* resource)
{
    const uint64_t key = uint64_t (uintptr_t (resource));
    if (key == 0)
        return 0;
    for (MapEntry& entry : g_mapped) {
        if (entry.resource.load (std::memory_order_acquire) != key)
            continue;
        const void* pointer =
            (const void*) (uintptr_t) entry.pointer.load (std::memory_order_relaxed);
        const uint32_t bytes = entry.bytes.load (std::memory_order_acquire);
        entry.pointer.store (0, std::memory_order_relaxed);
        entry.bytes.store (0, std::memory_order_relaxed);
        entry.resource.store (0, std::memory_order_release);
        if (pointer != nullptr && bytes > 0) {
            OnConstantBufferWrite (resource, 0, pointer, bytes);
            return bytes;
        }
        return 0;
    }
    return 0;
}

void OnConstantBufferBound (uint32_t slotKind, uint32_t startSlot, ID3D11Buffer* buffer)
{
    if (buffer == nullptr)
        return;
    // ⚠️ IT DOES NOT CLAIM A SLOT. Archicad binds far more buffers than it
    // updates, and letting a bind fill the table would push the handful that
    // actually carry per-frame data out of it. Only a captured WRITE claims an
    // entry; a bind merely labels one that already exists.
    Tracked* entry = Find (uint64_t (uintptr_t (buffer)));
    if (entry == nullptr)
        return;
    entry->shaderStage.store (slotKind, std::memory_order_relaxed);
    entry->bindSlot.store (startSlot, std::memory_order_relaxed);
}

void OnConstantBufferWrite (ID3D11Resource* resource, uint32_t byteOffset, const void* bytes,
                            uint32_t byteCount)
{
    if (resource == nullptr || bytes == nullptr || byteCount == 0)
        return;
    if (byteOffset >= kMaxTrackedBytes)
        return;
    const uint32_t copyBytes = std::min (byteCount, kMaxTrackedBytes - byteOffset);

    Tracked* entry = FindOrClaim (uint64_t (uintptr_t (resource)));
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
    g_referenceValid = Finite (g_referenceViewProj);
}

bool HasReference ()
{
    return g_referenceValid;
}

size_t Classify (Candidate* out, size_t max)
{
    g_regionsScored = 0;
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
        const uint32_t width = entry.byteWidth.load (std::memory_order_relaxed);
        if (width < 64)
            continue;
        if (!ReadStable (entry, snapshot, std::min (width, kMaxTrackedBytes)))
            continue;

        for (uint32_t offset = 0; offset + 64 <= width; offset += 64) {
            float stored[16];
            std::memcpy (stored, snapshot + offset, sizeof (stored));
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
                result.byteOffset = offset;
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

                // Insertion sort into a small best-of table. Keeping every scored
                // region would be thousands of rows a tick for no gain: what the
                // stage needs is the handful that came close.
                size_t position = foundCount;
                while (position > 0 && found[position - 1].maxPixelError > maxError) {
                    if (position < 16)
                        found[position] = found[position - 1];
                    --position;
                }
                if (position < 16) {
                    found[position] = result;
                    foundCount = std::min<size_t> (foundCount + 1, 16);
                }
            }
        }
    }

    const size_t written = std::min (foundCount, max);
    for (size_t i = 0; i < written; ++i)
        out[i] = found[i];
    g_best = (foundCount > 0) ? found[0] : Candidate {};
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
    stats.mapsSeen = g_mapsSeen.load (std::memory_order_relaxed);
    stats.mapsNotConstantBuffer = g_mapsNotConstantBuffer.load (std::memory_order_relaxed);
    stats.mapsTooLarge = g_mapsTooLarge.load (std::memory_order_relaxed);
    stats.mapsNoSlot = g_mapsNoSlot.load (std::memory_order_relaxed);
    stats.largestConstantBytes = g_largestConstantBytes.load (std::memory_order_relaxed);
    stats.bufferCacheFull = g_bufferCacheFull.load (std::memory_order_relaxed);
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
    for (BufferInfo& entry : g_bufferCache) {
        entry.resource.store (0, std::memory_order_relaxed);
        entry.byteWidth.store (0, std::memory_order_relaxed);
        entry.isConstantBuffer.store (0, std::memory_order_relaxed);
    }
    g_bufferCacheFull.store (false, std::memory_order_relaxed);
    for (MapEntry& entry : g_mapped) {
        entry.resource.store (0, std::memory_order_relaxed);
        entry.pointer.store (0, std::memory_order_relaxed);
        entry.bytes.store (0, std::memory_order_relaxed);
    }
    g_buffersTracked.store (0, std::memory_order_relaxed);
    g_mapsSeen.store (0, std::memory_order_relaxed);
    g_mapsNotConstantBuffer.store (0, std::memory_order_relaxed);
    g_mapsTooLarge.store (0, std::memory_order_relaxed);
    g_mapsNoSlot.store (0, std::memory_order_relaxed);
    g_largestConstantBytes.store (0, std::memory_order_relaxed);
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
