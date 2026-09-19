// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.

// See CameraFreshness.hpp.

#include "ArchViz/Dxgi/CameraFreshness.hpp"

#include <dxgi.h>

#include <atomic>
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
    return report;
}

void Reset ()
{
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
}

} // namespace freshness
} // namespace injection
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
