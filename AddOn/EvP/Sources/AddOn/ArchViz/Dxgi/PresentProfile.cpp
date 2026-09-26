// ArchViz/Dxgi/PresentProfile -- swap chain facts (buffer count, swap effect,
// format, size, scaling, alpha mode) and Present flag / sync-interval counts
// for the nominated chain, refreshed only when its identity changes.
// Bound by private/docs/architecture/diligent/OVERLAY-INVARIANTS.md: no locks,
// no heap allocation, no ACAPI calls and no heavy diagnostics on the Present
// thread; GetDesc/GetDesc1 are transient and gated on identity change only.

#include "ArchViz/Dxgi/PresentProfile.hpp"

#include <dxgi1_2.h>

#include <atomic>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace presentprofile {

namespace {

constexpr size_t kSyncIntervalBuckets = 5; // 0,1,2,3,>=4

// DXGI_PRESENT_* bits this module names individually; anything else only sets
// `otherFlags` and still folds into `flagsSeen`.
constexpr uint32_t kFlagDoNotSequence = 0x2;
constexpr uint32_t kFlagRestart = 0x4;
constexpr uint32_t kFlagDoNotWait = 0x8;
constexpr uint32_t kFlagRestrictToOutput = 0x40;
constexpr uint32_t kFlagUseDuration = 0x100;
constexpr uint32_t kFlagAllowTearing = 0x200;
constexpr uint32_t kKnownFlagMask =
    kFlagDoNotSequence | kFlagRestart | kFlagDoNotWait | kFlagRestrictToOutput | kFlagUseDuration | kFlagAllowTearing;

std::atomic<bool> g_enabled { false };
std::atomic<uint64_t> g_presents { 0 };
std::atomic<uint64_t> g_present1Calls { 0 };
std::atomic<uint64_t> g_syncInterval[kSyncIntervalBuckets] = {};
std::atomic<uint64_t> g_doNotSequence { 0 };
std::atomic<uint64_t> g_restart { 0 };
std::atomic<uint64_t> g_doNotWait { 0 };
std::atomic<uint64_t> g_restrictToOutput { 0 };
std::atomic<uint64_t> g_useDuration { 0 };
std::atomic<uint64_t> g_allowTearing { 0 };
std::atomic<uint64_t> g_otherFlags { 0 };
std::atomic<uint32_t> g_flagsSeen { 0 };
std::atomic<uint64_t> g_descQueries { 0 };
std::atomic<uint64_t> g_descFailures { 0 };

// Mirrors `Stats::chain` as individual atomics. `known` is written last with
// release and read first with acquire (seqlock-style, matching
// ImageTransferTrace's EventSlot): a reader that observes `known == true` is
// guaranteed to see every other field as of that publish, never a partial one.
struct SwapChainFactsAtomic {
    std::atomic<uint64_t> swapChain { 0 };
    std::atomic<uint32_t> bufferCount { 0 };
    std::atomic<uint32_t> swapEffect { 0 };
    std::atomic<uint32_t> bufferUsage { 0 };
    std::atomic<uint32_t> flags { 0 };
    std::atomic<uint32_t> format { 0 };
    std::atomic<uint32_t> width { 0 };
    std::atomic<uint32_t> height { 0 };
    std::atomic<uint32_t> sampleCount { 0 };
    std::atomic<bool> windowed { false };
    std::atomic<bool> desc1Known { false };
    std::atomic<uint32_t> scaling { 0 };
    std::atomic<uint32_t> alphaMode { 0 };
    std::atomic<bool> known { false };
};
SwapChainFactsAtomic g_chain;

// Transient GetDesc (+ QueryInterface(IDXGISwapChain1) -> GetDesc1 -> Release).
// No persistent COM reference is kept -- OVERLAY-INVARIANTS.md SS11: never
// cache a raw Archicad/DXGI resource pointer, and this holds none at all, not
// even within this one call. Called only from `OnPresent`, only when the
// identity check there decides a refresh is due.
void RefreshSwapChainFacts (IDXGISwapChain* swapChain, uint64_t identity)
{
    g_descQueries.fetch_add (1, std::memory_order_relaxed);

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED (swapChain->GetDesc (&desc))) {
        g_descFailures.fetch_add (1, std::memory_order_relaxed);
        return; // leave whatever facts were already published as the last-known state
    }

    bool desc1Known = false;
    uint32_t scaling = 0;
    uint32_t alphaMode = 0;
    IDXGISwapChain1* swapChain1 = nullptr;
    if (SUCCEEDED (swapChain->QueryInterface (__uuidof (IDXGISwapChain1), (void**) &swapChain1)) &&
        swapChain1 != nullptr) {
        DXGI_SWAP_CHAIN_DESC1 desc1 = {};
        if (SUCCEEDED (swapChain1->GetDesc1 (&desc1))) {
            scaling = uint32_t (desc1.Scaling);
            alphaMode = uint32_t (desc1.AlphaMode);
            desc1Known = true;
        }
        swapChain1->Release ();
    }

    // Every field written relaxed, THEN `known` released last -- see the
    // struct comment above.
    g_chain.swapChain.store (identity, std::memory_order_relaxed);
    g_chain.bufferCount.store (desc.BufferCount, std::memory_order_relaxed);
    g_chain.swapEffect.store (uint32_t (desc.SwapEffect), std::memory_order_relaxed);
    g_chain.bufferUsage.store (uint32_t (desc.BufferUsage), std::memory_order_relaxed);
    g_chain.flags.store (uint32_t (desc.Flags), std::memory_order_relaxed);
    g_chain.format.store (uint32_t (desc.BufferDesc.Format), std::memory_order_relaxed);
    g_chain.width.store (desc.BufferDesc.Width, std::memory_order_relaxed);
    g_chain.height.store (desc.BufferDesc.Height, std::memory_order_relaxed);
    g_chain.sampleCount.store (desc.SampleDesc.Count, std::memory_order_relaxed);
    g_chain.windowed.store (desc.Windowed != 0, std::memory_order_relaxed);
    g_chain.desc1Known.store (desc1Known, std::memory_order_relaxed);
    g_chain.scaling.store (scaling, std::memory_order_relaxed);
    g_chain.alphaMode.store (alphaMode, std::memory_order_relaxed);
    g_chain.known.store (true, std::memory_order_release);
}

} // namespace

void SetEnabled (bool enabled)
{
    g_enabled.store (enabled, std::memory_order_release);
}

bool Enabled ()
{
    return g_enabled.load (std::memory_order_acquire);
}

void Reset ()
{
    g_presents.store (0, std::memory_order_relaxed);
    g_present1Calls.store (0, std::memory_order_relaxed);
    for (std::atomic<uint64_t>& bucket : g_syncInterval)
        bucket.store (0, std::memory_order_relaxed);
    g_doNotSequence.store (0, std::memory_order_relaxed);
    g_restart.store (0, std::memory_order_relaxed);
    g_doNotWait.store (0, std::memory_order_relaxed);
    g_restrictToOutput.store (0, std::memory_order_relaxed);
    g_useDuration.store (0, std::memory_order_relaxed);
    g_allowTearing.store (0, std::memory_order_relaxed);
    g_otherFlags.store (0, std::memory_order_relaxed);
    g_flagsSeen.store (0, std::memory_order_relaxed);
    g_descQueries.store (0, std::memory_order_relaxed);
    g_descFailures.store (0, std::memory_order_relaxed);
    g_chain.swapChain.store (0, std::memory_order_relaxed);
    g_chain.bufferCount.store (0, std::memory_order_relaxed);
    g_chain.swapEffect.store (0, std::memory_order_relaxed);
    g_chain.bufferUsage.store (0, std::memory_order_relaxed);
    g_chain.flags.store (0, std::memory_order_relaxed);
    g_chain.format.store (0, std::memory_order_relaxed);
    g_chain.width.store (0, std::memory_order_relaxed);
    g_chain.height.store (0, std::memory_order_relaxed);
    g_chain.sampleCount.store (0, std::memory_order_relaxed);
    g_chain.windowed.store (false, std::memory_order_relaxed);
    g_chain.desc1Known.store (false, std::memory_order_relaxed);
    g_chain.scaling.store (0, std::memory_order_relaxed);
    g_chain.alphaMode.store (0, std::memory_order_relaxed);
    // `known` cleared LAST, release -- so a reader racing this against a fresh
    // `OnPresent` refresh sees either the fully-reset state or the
    // fully-refreshed one, never a mix (same publish rule as everywhere else).
    g_chain.known.store (false, std::memory_order_release);
}

void OnPresent (IDXGISwapChain* swapChain, unsigned int syncInterval, unsigned int flags, bool present1)
{
    if (!Enabled ())
        return;
    if (swapChain == nullptr)
        return;

    g_presents.fetch_add (1, std::memory_order_relaxed);
    if (present1)
        g_present1Calls.fetch_add (1, std::memory_order_relaxed);

    const size_t bucket =
        syncInterval < (kSyncIntervalBuckets - 1) ? size_t (syncInterval) : (kSyncIntervalBuckets - 1);
    g_syncInterval[bucket].fetch_add (1, std::memory_order_relaxed);

    if ((flags & kFlagDoNotSequence) != 0)
        g_doNotSequence.fetch_add (1, std::memory_order_relaxed);
    if ((flags & kFlagRestart) != 0)
        g_restart.fetch_add (1, std::memory_order_relaxed);
    if ((flags & kFlagDoNotWait) != 0)
        g_doNotWait.fetch_add (1, std::memory_order_relaxed);
    if ((flags & kFlagRestrictToOutput) != 0)
        g_restrictToOutput.fetch_add (1, std::memory_order_relaxed);
    if ((flags & kFlagUseDuration) != 0)
        g_useDuration.fetch_add (1, std::memory_order_relaxed);
    if ((flags & kFlagAllowTearing) != 0)
        g_allowTearing.fetch_add (1, std::memory_order_relaxed);
    if ((flags & ~kKnownFlagMask) != 0)
        g_otherFlags.fetch_add (1, std::memory_order_relaxed);
    g_flagsSeen.fetch_or (uint32_t (flags), std::memory_order_relaxed);

    // GetDesc/GetDesc1 only on an identity change (or after Reset, which
    // clears `known` and so looks like one) -- never once per Present.
    const uint64_t identity = uint64_t (uintptr_t (swapChain));
    const bool knownNow = g_chain.known.load (std::memory_order_acquire);
    const uint64_t cached = g_chain.swapChain.load (std::memory_order_relaxed);
    if (!knownNow || cached != identity)
        RefreshSwapChainFacts (swapChain, identity);
}

Stats GetStats ()
{
    Stats stats;
    stats.enabled = Enabled ();
    stats.presents = g_presents.load (std::memory_order_relaxed);
    stats.present1Calls = g_present1Calls.load (std::memory_order_relaxed);
    for (size_t i = 0; i < kSyncIntervalBuckets; ++i)
        stats.syncInterval[i] = g_syncInterval[i].load (std::memory_order_relaxed);
    stats.doNotSequence = g_doNotSequence.load (std::memory_order_relaxed);
    stats.restart = g_restart.load (std::memory_order_relaxed);
    stats.doNotWait = g_doNotWait.load (std::memory_order_relaxed);
    stats.restrictToOutput = g_restrictToOutput.load (std::memory_order_relaxed);
    stats.useDuration = g_useDuration.load (std::memory_order_relaxed);
    stats.allowTearing = g_allowTearing.load (std::memory_order_relaxed);
    stats.otherFlags = g_otherFlags.load (std::memory_order_relaxed);
    stats.flagsSeen = g_flagsSeen.load (std::memory_order_relaxed);
    stats.descQueries = g_descQueries.load (std::memory_order_relaxed);
    stats.descFailures = g_descFailures.load (std::memory_order_relaxed);

    // `known` loaded FIRST, acquire -- see the struct comment; only then are
    // the rest of the chain's fields read, and they are guaranteed consistent
    // with it.
    const bool known = g_chain.known.load (std::memory_order_acquire);
    stats.chain.known = known;
    if (known) {
        stats.chain.swapChain = g_chain.swapChain.load (std::memory_order_relaxed);
        stats.chain.bufferCount = g_chain.bufferCount.load (std::memory_order_relaxed);
        stats.chain.swapEffect = g_chain.swapEffect.load (std::memory_order_relaxed);
        stats.chain.bufferUsage = g_chain.bufferUsage.load (std::memory_order_relaxed);
        stats.chain.flags = g_chain.flags.load (std::memory_order_relaxed);
        stats.chain.format = g_chain.format.load (std::memory_order_relaxed);
        stats.chain.width = g_chain.width.load (std::memory_order_relaxed);
        stats.chain.height = g_chain.height.load (std::memory_order_relaxed);
        stats.chain.sampleCount = g_chain.sampleCount.load (std::memory_order_relaxed);
        stats.chain.windowed = g_chain.windowed.load (std::memory_order_relaxed);
        stats.chain.desc1Known = g_chain.desc1Known.load (std::memory_order_relaxed);
        stats.chain.scaling = g_chain.scaling.load (std::memory_order_relaxed);
        stats.chain.alphaMode = g_chain.alphaMode.load (std::memory_order_relaxed);
    }
    return stats;
}

} // namespace presentprofile
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
