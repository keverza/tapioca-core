// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.

// See CameraFreshness.hpp.

#include "ArchViz/Dxgi/CameraFreshness.hpp"

#include <dxgi.h>

#include <atomic>

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
    g_needsRedraw.store (true, std::memory_order_release);
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

bool TakeRedrawRequest ()
{
    return g_needsRedraw.exchange (false, std::memory_order_acq_rel);
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
}

} // namespace freshness
} // namespace injection
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
