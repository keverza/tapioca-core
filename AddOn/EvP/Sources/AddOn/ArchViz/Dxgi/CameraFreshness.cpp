// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.

// See CameraFreshness.hpp.

#include "ArchViz/Dxgi/CameraFreshness.hpp"

#include <dxgi.h>

#include <atomic>
#include <cstring>
#include <windows.h> // GetTickCount64 -- one read of a shared page, no syscall

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {
namespace freshness {
namespace {

// Packed width << 16 | height.
std::atomic<uint32_t> g_targetExtent { 0 };
std::atomic<uint32_t> g_acceptedExtent { 0 };
std::atomic<bool> g_needsRedraw { false };
std::atomic<uint64_t> g_suppressed { 0 };

const uint32_t kBuckets = 4; // 0, 1, 2, 3-or-more
std::atomic<uint64_t> g_histogram[kBuckets];
std::atomic<uint32_t> g_ageMax { 0 };
std::atomic<uint64_t> g_samples { 0 };

// When the OUTSTANDING request was raised, 0 if none is. See the header.
std::atomic<uint64_t> g_requestRaisedMs { 0 };
std::atomic<uint32_t> g_requestWaitMaxMs { 0 };
std::atomic<uint64_t> g_requestsTaken { 0 };

// REPEAT_SCENE's claim, partitioned. See the header.
std::atomic<uint64_t> g_repeatHeld { 0 };
std::atomic<uint64_t> g_repeatPassMoved { 0 };
std::atomic<uint64_t> g_repeatWindowMoved { 0 };

// ---- camera CONTENT ------------------------------------------------------
//
// ⚠️ ALL OF THESE ARE WRITTEN FROM ONE THREAD.
// `NoteCameraContent` runs inside the census readback, `NoteCameraAdopted` and
// `NoteRepeatScene` inside Present -- all on Archicad's render thread, and the
// census holds `ScopedInjectionGuard` while it resolves. They are atomic so the
// MAIN thread can read a coherent `Snapshot`, not to make the writes safe
// against each other.
std::atomic<uint64_t> g_contentSignature { 0 };
std::atomic<uint64_t> g_contentSerial { 0 };  // increments on every DECODE
std::atomic<uint64_t> g_contentChanges { 0 }; // ... of those, that differed
std::atomic<uint64_t> g_contentChangedMs { 0 };
std::atomic<uint64_t> g_contentDecodedMs { 0 };

std::atomic<uint64_t> g_acceptedSignature { 0 };
std::atomic<uint64_t> g_acceptedSerial { 0 };
std::atomic<uint64_t> g_acceptedMs { 0 };

std::atomic<uint64_t> g_camAdopted { 0 };
std::atomic<uint64_t> g_camSame { 0 };
std::atomic<uint64_t> g_camFreshNotAdopted { 0 };
std::atomic<uint64_t> g_serialLagMax { 0 };
std::atomic<uint32_t> g_msSinceLatestCaptureMax { 0 };
std::atomic<uint32_t> g_msSinceAcceptedChangedMax { 0 };

std::atomic<uint64_t> g_freshRunCurrent { 0 };
std::atomic<uint64_t> g_freshRunMax { 0 };
// What was true across the run currently open, so the recovery can say what
// changed when it ended.
std::atomic<uint64_t> g_freshRunStartMs { 0 };
std::atomic<uint64_t> g_freshRunStartSignature { 0 };
std::atomic<bool> g_freshRunPassMoved { false };
std::atomic<bool> g_freshRunWindowMoved { false };

std::atomic<uint64_t> g_snapshots { 0 };
std::atomic<uint64_t> g_lastSnapshotMs { 0 };
std::atomic<uint32_t> g_msSinceSnapshotMax { 0 };

std::atomic<uint64_t> g_recoveryRunLength { 0 };
std::atomic<uint32_t> g_recoveryMs { 0 };
std::atomic<bool> g_recoverySignatureChanged { false };
std::atomic<bool> g_recoveryPassMoved { false };
std::atomic<bool> g_recoveryWindowMoved { false };

void RaiseTo (std::atomic<uint64_t>& high, uint64_t value)
{
    uint64_t seen = high.load (std::memory_order_relaxed);
    while (value > seen && !high.compare_exchange_weak (seen, value, std::memory_order_relaxed)) {
    }
}

void RaiseTo32 (std::atomic<uint32_t>& high, uint32_t value)
{
    uint32_t seen = high.load (std::memory_order_relaxed);
    while (value > seen && !high.compare_exchange_weak (seen, value, std::memory_order_relaxed)) {
    }
}

// ⚠️ THE RUN IS CLOSED WHERE IT ENDS, NOT WHERE IT IS
// NOTICED. Every exit from FRESH_NOT_ADOPTED comes through here, so the
// recovery record describes the run that actually ended rather than whichever
// one happened to be open when the report was printed.
void CloseFreshRun ()
{
    const uint64_t run = g_freshRunCurrent.exchange (0, std::memory_order_relaxed);
    if (run == 0)
        return;
    RaiseTo (g_freshRunMax, run);
    if (run >= g_recoveryRunLength.load (std::memory_order_relaxed)) {
        g_recoveryRunLength.store (run, std::memory_order_relaxed);
        const uint64_t began = g_freshRunStartMs.load (std::memory_order_relaxed);
        g_recoveryMs.store (began == 0 ? 0u : uint32_t (::GetTickCount64 () - began), std::memory_order_relaxed);
        g_recoverySignatureChanged.store (g_contentSignature.load (std::memory_order_relaxed) !=
                                              g_freshRunStartSignature.load (std::memory_order_relaxed),
                                          std::memory_order_relaxed);
        g_recoveryPassMoved.store (g_freshRunPassMoved.load (std::memory_order_relaxed), std::memory_order_relaxed);
        g_recoveryWindowMoved.store (g_freshRunWindowMoved.load (std::memory_order_relaxed), std::memory_order_relaxed);
    }
    g_freshRunPassMoved.store (false, std::memory_order_relaxed);
    g_freshRunWindowMoved.store (false, std::memory_order_relaxed);
    g_freshRunStartMs.store (0, std::memory_order_relaxed);
}

} // namespace

void NoteTargetExtent (IDXGISwapChain* swapChain)
{
    if (swapChain == nullptr)
        return;
    DXGI_SWAP_CHAIN_DESC chain = {};
    if (FAILED (swapChain->GetDesc (&chain)))
        return;
    g_targetExtent.store (((chain.BufferDesc.Width & 0xffffu) << 16) | (chain.BufferDesc.Height & 0xffffu),
                          std::memory_order_relaxed);
}

void NoteAccepted ()
{
    g_acceptedExtent.store (g_targetExtent.load (std::memory_order_relaxed), std::memory_order_relaxed);
}

bool Stale ()
{
    const uint32_t now = g_targetExtent.load (std::memory_order_relaxed);
    const uint32_t when = g_acceptedExtent.load (std::memory_order_relaxed);
    return now != 0 && when != 0 && now != when;
}

void NoteSuppressed ()
{
    g_suppressed.fetch_add (1, std::memory_order_relaxed);
    // ⚠️ THE CLOCK STARTS ON THE FIRST SUPPRESSED PRESENT,
    // NOT ON THE LAST. The request is coalesced, so a hundred blank frames raise
    // one request; timestamping each of them would reset the clock every frame
    // and report a wait of nearly zero for a stall that lasted seconds -- which
    // is the measurement saying what it was built to detect cannot happen.
    if (!g_needsRedraw.exchange (true, std::memory_order_acq_rel))
        g_requestRaisedMs.store (::GetTickCount64 (), std::memory_order_relaxed);
}

void NoteAge (uint64_t presentGeneration, uint64_t snapshotGeneration)
{
    const uint64_t age = presentGeneration > snapshotGeneration ? presentGeneration - snapshotGeneration : 0;
    g_histogram[age < kBuckets ? uint32_t (age) : kBuckets - 1].fetch_add (1, std::memory_order_relaxed);
    g_samples.fetch_add (1, std::memory_order_relaxed);
    uint32_t seen = g_ageMax.load (std::memory_order_relaxed);
    while (uint32_t (age) > seen && !g_ageMax.compare_exchange_weak (seen, uint32_t (age), std::memory_order_relaxed)) {
    }
}

void NoteRepeatScene (bool usable, bool passMoved, bool windowMoved)
{
    if (!usable)
        return;
    if (passMoved)
        g_repeatPassMoved.fetch_add (1, std::memory_order_relaxed);
    else if (windowMoved)
        g_repeatWindowMoved.fetch_add (1, std::memory_order_relaxed);
    else
        g_repeatHeld.fetch_add (1, std::memory_order_relaxed);

    // ⚠️ AND THE SAME PRESENT IS CLASSIFIED BY CONTENT,
    // WHICH IS THE ONLY ONE OF THE TWO THAT CAN SEE A BUFFER REWRITTEN IN PLACE.
    // This branch is by definition a Present that KEPT the accepted camera, so
    // the question is whether it should have.
    const uint64_t latest = g_contentSignature.load (std::memory_order_relaxed);
    const uint64_t accepted = g_acceptedSignature.load (std::memory_order_relaxed);
    const uint64_t now = ::GetTickCount64 ();
    RaiseTo (g_serialLagMax,
             g_contentSerial.load (std::memory_order_relaxed) - g_acceptedSerial.load (std::memory_order_relaxed));
    const uint64_t decoded = g_contentDecodedMs.load (std::memory_order_relaxed);
    if (decoded != 0)
        RaiseTo32 (g_msSinceLatestCaptureMax, uint32_t (now - decoded));
    const uint64_t acceptedMs = g_acceptedMs.load (std::memory_order_relaxed);
    if (acceptedMs != 0)
        RaiseTo32 (g_msSinceAcceptedChangedMax, uint32_t (now - acceptedMs));

    if (accepted == 0 || latest == accepted) {
        g_camSame.fetch_add (1, std::memory_order_relaxed);
        CloseFreshRun ();
        return;
    }
    g_camFreshNotAdopted.fetch_add (1, std::memory_order_relaxed);
    if (g_freshRunCurrent.fetch_add (1, std::memory_order_relaxed) == 0) {
        g_freshRunStartMs.store (now, std::memory_order_relaxed);
        g_freshRunStartSignature.store (latest, std::memory_order_relaxed);
    }
    if (passMoved)
        g_freshRunPassMoved.store (true, std::memory_order_relaxed);
    if (windowMoved)
        g_freshRunWindowMoved.store (true, std::memory_order_relaxed);
}

void NoteCameraContent (const float* view16, const float* projection16, float vpX, float vpY, float vpW, float vpH)
{
    if (view16 == nullptr || projection16 == nullptr)
        return;

    // FNV-1a over the raw bit patterns. The bytes were copied verbatim out of
    // Archicad's ring, so equal content is bit-equal content and there is no
    // floating-point noise to quantise away.
    uint64_t hash = 1469598103934665603ull;
    const auto eat = [&hash] (float value) {
        uint32_t bits = 0;
        std::memcpy (&bits, &value, sizeof (bits));
        for (int byte = 0; byte < 4; ++byte) {
            hash ^= uint64_t ((bits >> (byte * 8)) & 0xffu);
            hash *= 1099511628211ull;
        }
    };
    for (int i = 0; i < 16; ++i)
        eat (view16[i]);
    for (int i = 0; i < 16; ++i)
        eat (projection16[i]);
    eat (vpX);
    eat (vpY);
    eat (vpW);
    eat (vpH);
    if (hash == 0)
        hash = 1; // 0 means "nothing decoded yet" everywhere below

    const uint64_t now = ::GetTickCount64 ();
    g_contentSerial.fetch_add (1, std::memory_order_relaxed);
    g_contentDecodedMs.store (now, std::memory_order_relaxed);
    if (g_contentSignature.exchange (hash, std::memory_order_relaxed) != hash) {
        g_contentChanges.fetch_add (1, std::memory_order_relaxed);
        g_contentChangedMs.store (now, std::memory_order_relaxed);
    }
}

void NoteAuthoritativeSnapshot ()
{
    g_snapshots.fetch_add (1, std::memory_order_relaxed);
    const uint64_t now = ::GetTickCount64 ();
    const uint64_t previous = g_lastSnapshotMs.exchange (now, std::memory_order_relaxed);
    // ⚠️ THE GAP IS MEASURED WHEN IT CLOSES, NOT WHILE
    // IT IS OPEN. A gap that is still running has no length yet, and taking the
    // maximum of a running gap on every Present would report the session's age.
    // The live one is reported separately, from `Snapshot`.
    if (previous != 0)
        RaiseTo32 (g_msSinceSnapshotMax, uint32_t (now - previous));
}

void NoteCameraAdopted ()
{
    g_camAdopted.fetch_add (1, std::memory_order_relaxed);
    g_acceptedSignature.store (g_contentSignature.load (std::memory_order_relaxed), std::memory_order_relaxed);
    g_acceptedSerial.store (g_contentSerial.load (std::memory_order_relaxed), std::memory_order_relaxed);
    g_acceptedMs.store (::GetTickCount64 (), std::memory_order_relaxed);
    CloseFreshRun ();
}

uint32_t TargetEpoch ()
{
    return g_targetExtent.load (std::memory_order_relaxed);
}

bool TakeRedrawRequest ()
{
    if (!g_needsRedraw.exchange (false, std::memory_order_acq_rel))
        return false;

    const uint64_t raised = g_requestRaisedMs.exchange (0, std::memory_order_relaxed);
    if (raised != 0) {
        const uint64_t waited = ::GetTickCount64 () - raised;
        uint32_t seen = g_requestWaitMaxMs.load (std::memory_order_relaxed);
        while (uint32_t (waited) > seen &&
               !g_requestWaitMaxMs.compare_exchange_weak (seen, uint32_t (waited), std::memory_order_relaxed)) {
        }
    }
    g_requestsTaken.fetch_add (1, std::memory_order_relaxed);
    return true;
}

Report Snapshot ()
{
    Report report;
    report.age0 = g_histogram[0].load (std::memory_order_relaxed);
    report.age1 = g_histogram[1].load (std::memory_order_relaxed);
    report.age2 = g_histogram[2].load (std::memory_order_relaxed);
    report.age3plus = g_histogram[3].load (std::memory_order_relaxed);
    report.ageMax = g_ageMax.load (std::memory_order_relaxed);
    report.samples = g_samples.load (std::memory_order_relaxed);
    report.suppressed = g_suppressed.load (std::memory_order_relaxed);
    report.redrawWaitMaxMs = g_requestWaitMaxMs.load (std::memory_order_relaxed);
    report.redrawsTaken = g_requestsTaken.load (std::memory_order_relaxed);
    report.repeatHeld = g_repeatHeld.load (std::memory_order_relaxed);
    report.repeatPassMoved = g_repeatPassMoved.load (std::memory_order_relaxed);
    report.repeatWindowMoved = g_repeatWindowMoved.load (std::memory_order_relaxed);
    report.camAdopted = g_camAdopted.load (std::memory_order_relaxed);
    report.camSame = g_camSame.load (std::memory_order_relaxed);
    report.camFreshNotAdopted = g_camFreshNotAdopted.load (std::memory_order_relaxed);
    report.captureSerial = g_contentSerial.load (std::memory_order_relaxed);
    report.contentDecodes = g_contentSerial.load (std::memory_order_relaxed);
    report.contentChanges = g_contentChanges.load (std::memory_order_relaxed);
    report.serialLagMax = g_serialLagMax.load (std::memory_order_relaxed);
    report.msSinceLatestCaptureMax = g_msSinceLatestCaptureMax.load (std::memory_order_relaxed);
    report.msSinceAcceptedChangedMax = g_msSinceAcceptedChangedMax.load (std::memory_order_relaxed);
    report.freshRunMax = g_freshRunMax.load (std::memory_order_relaxed);
    report.freshRunCurrent = g_freshRunCurrent.load (std::memory_order_relaxed);
    report.snapshots = g_snapshots.load (std::memory_order_relaxed);
    const uint64_t lastSnapshot = g_lastSnapshotMs.load (std::memory_order_relaxed);
    report.msSinceSnapshot = lastSnapshot == 0 ? 0u : uint32_t (::GetTickCount64 () - lastSnapshot);
    report.msSinceSnapshotMax = g_msSinceSnapshotMax.load (std::memory_order_relaxed);
    report.recoveryRunLength = g_recoveryRunLength.load (std::memory_order_relaxed);
    report.recoveryMs = g_recoveryMs.load (std::memory_order_relaxed);
    report.recoverySignatureChanged = g_recoverySignatureChanged.load (std::memory_order_relaxed);
    report.recoveryPassMoved = g_recoveryPassMoved.load (std::memory_order_relaxed);
    report.recoveryWindowMoved = g_recoveryWindowMoved.load (std::memory_order_relaxed);
    return report;
}

// The matrix ledger. See CameraFreshness.hpp.
const size_t kMatrixLedgerCapacity = 24;
GroupMatrices g_ledger[kMatrixLedgerCapacity];
size_t g_ledgerUsed = 0;

// The scene epoch gate. See CameraFreshness.hpp.
std::atomic<uint64_t> g_cameraEpoch { 0 };
std::atomic<uint64_t> g_presentedEpoch { 0 };
std::atomic<uint64_t> g_gateMatched { 0 };
std::atomic<uint64_t> g_gateMismatched { 0 };
std::atomic<uint64_t> g_gateSuppressed { 0 };
std::atomic<uint64_t> g_gateBehindMax { 0 };
std::atomic<uint64_t> g_gateAheadMax { 0 };
std::atomic<bool> g_gateEnabled { false };

void Reset ()
{
    g_ledgerUsed = 0;
    // Section 10: a run that inherits the previous one's gate is a run whose
    // evidence nobody can trust.
    g_gateEnabled.store (false, std::memory_order_release);
    g_cameraEpoch.store (0, std::memory_order_release);
    g_presentedEpoch.store (0, std::memory_order_release);
    g_gateMatched.store (0, std::memory_order_relaxed);
    g_gateMismatched.store (0, std::memory_order_relaxed);
    g_gateSuppressed.store (0, std::memory_order_relaxed);
    g_gateBehindMax.store (0, std::memory_order_relaxed);
    g_gateAheadMax.store (0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kBuckets; ++i)
        g_histogram[i].store (0, std::memory_order_relaxed);
    g_ageMax.store (0, std::memory_order_relaxed);
    g_samples.store (0, std::memory_order_relaxed);
    g_targetExtent.store (0, std::memory_order_relaxed);
    g_acceptedExtent.store (0, std::memory_order_relaxed);
    g_needsRedraw.store (false, std::memory_order_relaxed);
    g_suppressed.store (0, std::memory_order_relaxed);
    g_requestRaisedMs.store (0, std::memory_order_relaxed);
    g_requestWaitMaxMs.store (0, std::memory_order_relaxed);
    g_requestsTaken.store (0, std::memory_order_relaxed);
    g_repeatHeld.store (0, std::memory_order_relaxed);
    g_repeatPassMoved.store (0, std::memory_order_relaxed);
    g_repeatWindowMoved.store (0, std::memory_order_relaxed);
    g_contentSignature.store (0, std::memory_order_relaxed);
    g_contentSerial.store (0, std::memory_order_relaxed);
    g_contentChanges.store (0, std::memory_order_relaxed);
    g_contentChangedMs.store (0, std::memory_order_relaxed);
    g_contentDecodedMs.store (0, std::memory_order_relaxed);
    g_acceptedSignature.store (0, std::memory_order_relaxed);
    g_acceptedSerial.store (0, std::memory_order_relaxed);
    g_acceptedMs.store (0, std::memory_order_relaxed);
    g_camAdopted.store (0, std::memory_order_relaxed);
    g_camSame.store (0, std::memory_order_relaxed);
    g_camFreshNotAdopted.store (0, std::memory_order_relaxed);
    g_serialLagMax.store (0, std::memory_order_relaxed);
    g_msSinceLatestCaptureMax.store (0, std::memory_order_relaxed);
    g_msSinceAcceptedChangedMax.store (0, std::memory_order_relaxed);
    g_freshRunCurrent.store (0, std::memory_order_relaxed);
    g_freshRunMax.store (0, std::memory_order_relaxed);
    g_freshRunStartMs.store (0, std::memory_order_relaxed);
    g_freshRunStartSignature.store (0, std::memory_order_relaxed);
    g_freshRunPassMoved.store (false, std::memory_order_relaxed);
    g_freshRunWindowMoved.store (false, std::memory_order_relaxed);
    g_snapshots.store (0, std::memory_order_relaxed);
    g_lastSnapshotMs.store (0, std::memory_order_relaxed);
    g_msSinceSnapshotMax.store (0, std::memory_order_relaxed);
    g_recoveryRunLength.store (0, std::memory_order_relaxed);
    g_recoveryMs.store (0, std::memory_order_relaxed);
    g_recoverySignatureChanged.store (false, std::memory_order_relaxed);
    g_recoveryPassMoved.store (false, std::memory_order_relaxed);
    g_recoveryWindowMoved.store (false, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// The scene epoch gate. See CameraFreshness.hpp.
// ---------------------------------------------------------------------------

void NoteCameraEpoch (uint64_t sceneGeneration)
{
    g_cameraEpoch.store (sceneGeneration, std::memory_order_release);
}

bool EpochGateAllows (uint64_t presentedGeneration)
{
    g_presentedEpoch.store (presentedGeneration, std::memory_order_release);
    const uint64_t camera = g_cameraEpoch.load (std::memory_order_acquire);
    // ⚠️ BEFORE THE FIRST OF EITHER THERE IS NOTHING TO
    // COMPARE, and a gate that refuses on absent evidence would suppress the
    // overlay for the whole of a session that never produced a scene pass --
    // which is the stationary case, not a fault.
    if (camera == 0 || presentedGeneration == 0) {
        g_gateMatched.fetch_add (1, std::memory_order_relaxed);
        return true;
    }
    if (camera == presentedGeneration) {
        g_gateMatched.fetch_add (1, std::memory_order_relaxed);
        return true;
    }
    g_gateMismatched.fetch_add (1, std::memory_order_relaxed);
    // ⚠️ BEHIND AND AHEAD ARE DIFFERENT FAULTS AND ARE
    // COUNTED APART. Behind is the overlay drawing an older camera than the
    // image -- the lag. Ahead is the overlay drawing a camera from a pass whose
    // pixels have not been presented yet, which would mean the image on screen
    // is not the pass we think it is, and no amount of camera work fixes that.
    if (camera < presentedGeneration) {
        const uint64_t behind = presentedGeneration - camera;
        uint64_t worst = g_gateBehindMax.load (std::memory_order_relaxed);
        while (behind > worst && !g_gateBehindMax.compare_exchange_weak (worst, behind))
            ;
    }
    else {
        const uint64_t ahead = camera - presentedGeneration;
        uint64_t worst = g_gateAheadMax.load (std::memory_order_relaxed);
        while (ahead > worst && !g_gateAheadMax.compare_exchange_weak (worst, ahead))
            ;
    }
    if (!g_gateEnabled.load (std::memory_order_acquire))
        return true;
    g_gateSuppressed.fetch_add (1, std::memory_order_relaxed);
    return false;
}

void SetEpochGate (bool enabled)
{
    g_gateEnabled.store (enabled, std::memory_order_release);
}

bool EpochGate ()
{
    return g_gateEnabled.load (std::memory_order_acquire);
}

EpochGateReport GetEpochGate ()
{
    EpochGateReport report;
    report.matched = g_gateMatched.load (std::memory_order_relaxed);
    report.mismatched = g_gateMismatched.load (std::memory_order_relaxed);
    report.suppressed = g_gateSuppressed.load (std::memory_order_relaxed);
    report.behindMax = g_gateBehindMax.load (std::memory_order_relaxed);
    report.aheadMax = g_gateAheadMax.load (std::memory_order_relaxed);
    report.cameraEpoch = g_cameraEpoch.load (std::memory_order_relaxed);
    report.presentedEpoch = g_presentedEpoch.load (std::memory_order_relaxed);
    report.enabled = g_gateEnabled.load (std::memory_order_relaxed);
    return report;
}

void NoteGroupMatrices (uint32_t groupId, uint32_t occurrence, uint64_t generation, const float view[16],
                        const float projection[16])
{
    for (size_t i = 0; i < g_ledgerUsed; ++i) {
        if (g_ledger[i].groupId == groupId)
            return; // the FIRST decode, so the set can be compared as one reading
    }
    if (g_ledgerUsed >= kMatrixLedgerCapacity)
        return;
    GroupMatrices& entry = g_ledger[g_ledgerUsed];
    entry.groupId = groupId;
    entry.occurrence = occurrence;
    entry.generation = generation;
    for (size_t i = 0; i < 16; ++i) {
        entry.view[i] = view[i];
        entry.projection[i] = projection[i];
    }
    ++g_ledgerUsed;
}

size_t GetGroupMatrices (GroupMatrices* out, size_t capacity)
{
    const size_t count = g_ledgerUsed < capacity ? g_ledgerUsed : capacity;
    for (size_t i = 0; i < count; ++i)
        out[i] = g_ledger[i];
    return count;
}

size_t GroupMatrixCount ()
{
    return g_ledgerUsed;
}

} // namespace freshness
} // namespace injection
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
