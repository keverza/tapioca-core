#ifndef EVP_ARCHVIZ_DXGI_CONTEXTEVENTRING_HPP
#define EVP_ARCHVIZ_DXGI_CONTEXTEVENTRING_HPP

// The fixed ring the context detours record into, and the drain that gets it
// back out (PLAT-RE153, docs/architecture/api/HANDOFF-OverlayPatch.md stage 1).
//
// WHY IT IS ITS OWN FILE. `ContextHook.cpp` owns the vtable protocol -- which
// slots are patched, how they are proven, how they are put back. This owns a
// different one: how a render thread hands a fixed-size record to the main
// thread without a lock, and how the main thread gets it onto the nav log's
// timeline. The two share nothing but the `ContextEvent` struct, they fail in
// unrelated ways, and keeping them together is what put that file over the size
// cap twice.
//
// ⚠️ RESERVE, FILL, PUBLISH -- NOT ONE COUNTER. `PresentHook` shipped the
// one-counter version and it let the main thread read a slot the render thread
// was still filling, producing a sample built from one row's timestamp and
// another's payload. A reader trusts nothing past `published`.
//
// ⚠️ AN OVERRUN IS COUNTED, NEVER SILENT. This ring holds one or two frames of a
// busy scene where the present hook's holds half a minute, because these calls
// arrive in the thousands per frame against Present's one. A dropped event looks
// exactly like a call Archicad did not make -- which is the single conclusion
// this whole hook exists to draw -- so `Dropped()` is what stops an absent
// constant-buffer write being read as "Archicad does not upload one here".
//
// THREAD SAFETY: `Record` is called from Archicad's render thread inside a
// detour. It allocates nothing, takes no lock and never calls ACAPI. Everything
// else is main thread.

#include "ArchViz/Dxgi/ContextHook.hpp"   // ContextEvent, ContextSlot

#include <cstddef>
#include <cstdint>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace eventring {

// RENDER THREAD.
void Record (ContextSlot slot, uint64_t handle, uint32_t a, uint32_t b, uint32_t c);

// MAIN THREAD. Oldest first; marks what it copies as consumed.
size_t Drain (ContextEvent* out, size_t max);

// MAIN THREAD. Empty it and zero the counters -- called when the hook installs,
// so a run never inherits the previous one's events.
void Reset ();

uint64_t Recorded ();
uint64_t Dropped ();
bool     HalfFull ();

// MAIN THREAD. Drain to the nav log as `source=ctx` rows. Does file IO, so it is
// never called from a detour.
void Flush ();

}   // namespace eventring
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv

#endif
