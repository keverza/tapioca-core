#ifndef EVP_ARCHVIZ_DXGI_PRESENTHOOK_HPP
#define EVP_ARCHVIZ_DXGI_PRESENTHOOK_HPP

// Watching Archicad's own frames go out (PLAT-RE78). DIAGNOSTIC ONLY -- this
// file renders nothing and must never learn how.
//
// WHY IT EXISTS. Every number the camera-sync ladder has produced so far is
// measured against OUR poll of Archicad's camera, so the best it can say is how
// stale the overlay is relative to that poll. It cannot see the two things that
// decide whether 1:1 is even reachable:
//
//   1. WHEN Archicad actually presents a frame. Without that there is no frame
//      clock, so the predictor's horizon (currently a hard-coded 31 ms) is a
//      guess, and "one frame behind" cannot be distinguished from "three".
//   2. WHETHER our overlay's frames and Archicad's are paired at all. Two swap
//      chains are composited by DWM on its own schedule; the desync analysis is
//      blind to that extra frame by construction and says so.
//
// The 2026-08-13 runs left a specific question this answers. Lag sat at 22-28 ms
// across `legacy`, `wake` and `predict` alike, while the ACAPI read measured
// 0.1-1.1 ms and the wake path put samples in at input priority. Something is
// holding a ~25 ms floor that is not the read and not the wake source. Either
// Archicad presents on a cadence we keep just missing -- which this measures --
// or the cost is downstream in composition, which only compositing into
// Archicad's own back buffer (PLAT-RE79) can remove.
//
// ⚠️ IT IS A VTABLE SWAP, NOT AN INLINE DETOUR, and that is a deliberate choice.
// IDXGISwapChain is a COM interface: its methods are reached through a pointer
// table, so redirecting one is a single pointer write behind VirtualProtect. An
// inline detour (MinHook and friends) rewrites the first bytes of the function
// itself -- it needs a disassembler to relocate them, it races with any other
// product that hooked the same function, and it fails in ways that crash rather
// than misbehave. The vtable swap needs no third-party code, is exactly
// reversible, and can be verified by comparing pointers.
//
// ⚠️ THE VTABLE IS SHARED BY EVERY SWAP CHAIN IN THE PROCESS, ours included.
// There is one table per interface implementation, not one per object. So this
// sees Archicad's frames AND the overlay's, and every entry is stamped with the
// swap chain pointer to tell them apart. It also means a bug here breaks our own
// rendering as well as Archicad's, which is why ExperimentGuard gates the mode.
//
// ⚠️ PRESENT IS CALLED FROM A RENDER THREAD, POSSIBLY MORE THAN ONE. Everything
// the detour touches is atomic, it takes no lock, it allocates nothing, and it
// never calls ACAPI. It records into a fixed ring and returns. Reading the ring
// out is the main thread's job.
//
// ⚠️ UNHOOKING IS THE DANGEROUS HALF, NOT HOOKING. Restoring the pointer does not
// wait for calls already inside the detour, and if the DLL unloads while one is
// in flight, Windows returns into freed code. `Remove` restores the pointers and
// then waits for the in-flight counter to reach zero before returning. The same
// hazard as a surviving hook or window procedure, and the same rule: nothing we
// installed may outlive the module.

#include <cstdint>
#include <string>

namespace geomsrv {
namespace archviz {
namespace dxgi {

// MAIN THREAD. Creates a throwaway device to discover the vtable, then patches
// it. False with `error` filled if any step failed, having undone whatever it
// managed -- a partial install is never left behind.
bool InstallPresentHook (std::string& error);

// MAIN THREAD. Idempotent. Restores the vtable and waits for in-flight detour
// calls to drain before returning.
void RemovePresentHook ();

bool PresentHookInstalled ();

// Tell the hook which swap chain is OURS, so its rows can say so.
//
// ⚠️ WITHOUT THIS THE FRAME CLOCK HAS TO BE GUESSED AT, and the guess is exactly
// the kind that looks safe and decides the next rung. The first hookdiag run
// produced two chains -- one at a rock-steady 59 Hz, one irregular at a median
// 50 Hz -- and which of them is Archicad's changes the reading completely: a
// steady 59 Hz Archicad means we are missing a fixed cadence, while an irregular
// Archicad means there is no cadence to hit. Our overlay renders continuously
// and Archicad redraws on demand, so the shapes are suggestive; suggestive is
// not the same as labelled. Called from the render thread once the target has a
// swap chain; 0 clears it.
void SetOwnSwapChain (uint64_t swapChain);

// The HWND a swap chain presents into, or 0 if it has not been asked yet.
//
// ⚠️ IT IS ASKED ONCE PER CHAIN, NOT PER FRAME, and that is the only reason it
// is safe. GetDesc is a COM call on an object we are inside a method of, on a
// thread we do not own, and the header's own rule forbids it on the hot path --
// so the answer is cached on the first Present from each chain and never asked
// again. Without it, "Archicad's swap chain" is whichever one presents most,
// which is an inference: any other active surface in the process can win it.
uint64_t SwapChainWindow (uint64_t swapChain);

// What the detour has seen. Counters are cumulative since Install.
struct PresentStats {
    bool     installed = false;
    uint64_t presentCalls = 0;
    uint64_t present1Calls = 0;
    uint64_t resizeCalls = 0;
    // The swap chain presenting most often that is NOT ours -- Archicad's, in
    // practice. Reported as an integer so it can cross the bus and be matched
    // against the log's own rows.
    uint64_t busiestSwapChain = 0;
    uint64_t busiestFrameCount = 0;
    // Median and p95 microseconds between consecutive presents of the busiest
    // swap chain: Archicad's frame clock, which nothing else can see.
    uint64_t medianFrameUs = 0;
    uint64_t p95FrameUs = 0;
};
PresentStats GetPresentStats ();

// Write the ring's contents to the nav log as `source=present` rows, then clear
// it. MAIN THREAD -- it does file IO.
void FlushPresentLog ();

// The same, but only once the ring is half full. Called from the camera-sync
// tick so a long run is written out as it goes: the ring holds about 30 s and a
// matrix run is minutes, so flushing only at teardown would keep the last
// gesture and silently drop the rest.
void FlushPresentLogIfFilling ();

}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv

#endif
