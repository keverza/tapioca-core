#ifndef EVP_ARCHVIZ_DXGI_CONTEXTHOOKSHARED_HPP
#define EVP_ARCHVIZ_DXGI_CONTEXTHOOKSHARED_HPP

// INTERNAL to the context hook. Not part of its interface, and nothing outside
// `ContextHook.cpp` and `ContextHookDetours.cpp` may include it.
//
// WHY IT EXISTS. The hook has two halves that grow at different rates. One is a
// lifecycle: discover Archicad's context, verify the patch profile, swap the
// table, prove it, drain and restore. The other is the detours themselves, and
// that half keeps growing — every question the discovery runs raise ("is the
// scene drawn here at all?", "is it played back from a deferred context?") is
// answered by adding one more. Keeping them in one translation unit pushed it
// over the size cap three times in one day, and each time the cheapest fix was
// to shave a comment, which is the wrong thing to shave.
//
// ⚠️ THESE ARE DELIBERATELY NOT IN AN ANONYMOUS NAMESPACE, which is the whole
// point of the file: two translation units must see the SAME filter, the same
// originals table and the same counters. An anonymous namespace would give each
// its own copy, the detours would record into counters nobody reads, and the
// symptom would be a hook that installs cleanly and reports zeroes — which is
// precisely the failure this rung has already spent a day chasing for a
// different reason.

#include "ArchViz/Dxgi/ContextHook.hpp"

#include <atomic>
#include <cstdint>

struct ID3D11Buffer;
struct ID3D11DeviceContext;
struct ID3D11DepthStencilView;
struct ID3D11RenderTargetView;
struct ID3D11Resource;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace hookshared {

// Which vtable slot each ContextSlot patches. Fixed by the COM ABI and taken
// from `d3d11.h`'s own `ID3D11DeviceContextVtbl`; see ContextHook.hpp for why
// they are still checked rather than trusted.
extern const size_t kSlotIndex[size_t (ContextSlot::Count)];

// One past the highest index patched, which is the writable window the swap
// needs. Keeping it tight is what stops a VirtualProtect over half of d3d11.dll.
extern const size_t kVtableSlots;

// The detour to install in each slot. Defined beside the detours themselves.
extern void* const kDetour[size_t (ContextSlot::Count)];

// ⚠️ ONE ARRAY INDEXED BY SLOT, NOT ONE NAMED POINTER PER METHOD. The named
// version needed two parallel switch statements to get a value in and back out,
// and its only failure mode was a slot present in one and forgotten in the
// other. Each detour casts its own entry at the call site, where the type is
// already known.
//
// ⚠️ EVERY DETOUR READS ITS ENTRY INTO A LOCAL BEFORE CALLING IT. `Remove` nulls
// these, and a null call on Archicad's render thread takes Archicad with it.
//
// ⚠️ ATOMIC BECAUSE `RepairContextHook` REWRITES THESE WHILE DETOURS ARE RUNNING.
// The repair runs on Archicad's render thread once per present and takes the
// value it finds in a re-pointed slot as the new original; meanwhile OUR OWN
// renderer is driving its own context through the same detours on a different
// thread, reading the same array. A plain `void*` there is a data race -- benign
// on x86, where an aligned pointer store cannot tear, but undefined by the
// language and exactly the kind of thing a future compiler hoists out of a loop.
// `relaxed` compiles to the same `mov` the plain read did, so the correctness is
// free: no ordering is needed, only indivisibility.
extern std::atomic<void*> g_original[size_t (ContextSlot::Count)];

template <typename Fn>
Fn OriginalOf (ContextSlot slot)
{
    return reinterpret_cast<Fn> (g_original[size_t (slot)].load (std::memory_order_relaxed));
}

// The drain counter. Incremented on entry to every detour and decremented on
// exit; `Remove` restores the pointers and then spins until it reads zero,
// because a call already inside a detour would otherwise return into a DLL that
// has unloaded.
extern std::atomic<int32_t> g_inFlight;

// The filter: which context is Archicad's, and which is the throwaway the
// install self-tests against. Everything else the detours see is ignored.
extern std::atomic<uint64_t> g_archicadContext;
extern std::atomic<uint64_t> g_selfTestContext;

extern std::atomic<bool>     g_slotEnabled[size_t (ContextSlot::Count)];
extern std::atomic<uint64_t> g_perSlot[size_t (ContextSlot::Count)];
extern std::atomic<uint64_t> g_selfTestSeen[size_t (ContextSlot::Count)];
extern std::atomic<uint64_t> g_calls;

// ⚠️ WHEN THE FIRST AND LAST ARCHICAD CALL ARRIVED, in QPC microseconds. These
// exist to separate the two stories the seventh run left standing, which demand
// opposite work and look identical in a total:
//
//   * the counts are spread across the whole run -- Archicad really does make
//     four draws in an orbit, the back buffer is filled by something outside
//     D3D11, and the contingency section is next;
//   * the counts all land in the first few milliseconds and then stop -- the
//     detours were bypassed or undone after one frame, the numbers describe our
//     instrument rather than Archicad, and the fix is here.
//
// A total of four cannot tell those apart. A span can.
extern std::atomic<uint64_t> g_firstCallUs;
extern std::atomic<uint64_t> g_lastCallUs;

// ⚠️ ON ITS OWN CACHE LINE. This is the one counter written by a thread that is
// NOT Archicad's -- our own renderer drives its context through detours too --
// so sharing a line with `g_calls` would have two render threads invalidating
// each other's cache thousands of times a frame, and the cost would land as
// Archicad frame time.
extern std::atomic<uint64_t> g_otherContextCalls;

enum class Audience { Ignore, Archicad, SelfTest };

bool     SlotOn (ContextSlot slot);
Audience Who (ID3D11DeviceContext* context, ContextSlot slot);

}   // namespace hookshared
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv

#endif
