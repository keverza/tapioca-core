#ifndef EVP_ARCHVIZ_DXGI_PRESENTPROFILE_HPP
#define EVP_ARCHVIZ_DXGI_PRESENTPROFILE_HPP

// ArchViz/Dxgi/PresentProfile -- swap chain facts (buffer count, swap effect,
// format, size, scaling, alpha mode) and Present flag / sync-interval counts
// for the nominated chain, refreshed only when its identity changes.
// Bound by private/docs/architecture/diligent/OVERLAY-INVARIANTS.md: no locks,
// no heap allocation, no ACAPI calls and no heavy diagnostics on the Present
// thread; GetDesc/GetDesc1 are transient and gated on identity change only.

#include <cstdint>

struct IDXGISwapChain;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace presentprofile {

struct SwapChainFacts {
    bool known = false;
    uint64_t swapChain = 0;
    uint32_t bufferCount = 0;
    uint32_t swapEffect = 0; // DXGI_SWAP_EFFECT
    uint32_t bufferUsage = 0;
    uint32_t flags = 0;  // DXGI_SWAP_CHAIN_FLAG bits
    uint32_t format = 0; // DXGI_FORMAT
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sampleCount = 0;
    bool windowed = false;
    bool desc1Known = false;
    uint32_t scaling = 0;   // DXGI_SCALING
    uint32_t alphaMode = 0; // DXGI_ALPHA_MODE
};

struct Stats {
    bool enabled = false;
    uint64_t presents = 0; // nominated Present + Present1 calls seen (TEST excluded upstream)
    uint64_t present1Calls = 0;
    uint64_t syncInterval[5] = {}; // 0,1,2,3,>=4
    uint64_t doNotSequence = 0;    // DXGI_PRESENT_DO_NOT_SEQUENCE 0x2
    uint64_t restart = 0;          // 0x4
    uint64_t doNotWait = 0;        // 0x8
    uint64_t restrictToOutput = 0; // 0x40
    uint64_t useDuration = 0;      // 0x100
    uint64_t allowTearing = 0;     // 0x200
    uint64_t otherFlags = 0;       // any other bit set
    uint32_t flagsSeen = 0;        // OR of every flags value
    uint64_t descQueries = 0;
    uint64_t descFailures = 0;
    SwapChainFacts chain;
};

void SetEnabled (bool enabled); // MAIN
bool Enabled ();
void Reset (); // MAIN, after the provenance drain

// PRESENT THREAD, nominated chain only, inside the active PassProvenance Present interval.
void OnPresent (IDXGISwapChain* swapChain, unsigned int syncInterval, unsigned int flags, bool present1);

Stats GetStats (); // MAIN

} // namespace presentprofile
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
