#ifndef EVP_ARCHVIZ_DXGI_CONTEXTHOOK_HPP
#define EVP_ARCHVIZ_DXGI_CONTEXTHOOK_HPP

// Watching what Archicad tells the GPU, on the frame it tells it (PLAT-RE153,
// docs/architecture/api/HANDOFF-OverlayPatch.md stage 1). DISCOVERY ONLY -- this
// file renders nothing and must never learn how.
//
// WHY IT EXISTS. Four rungs of the camera-sync ladder were built and measured --
// sample sooner, predict, reproject at blit time, composite into Archicad's back
// buffer -- and a ~25 ms floor survived all four, visible by eye as trailing
// during a drag and a snap at mouse-up. The cause is structural and none of them
// touch it: the overlay's camera comes from `ACAPI_View_Get3DProjectionSets`, on
// the main thread, AFTER Archicad has already drawn with that camera. No amount
// of freshness, extrapolation or compositing can close a frame we only learn
// about once it is over.
//
// The one construction that removes the cause is to stop reconstructing
// Archicad's camera and read the transform ARCHICAD ITSELF uploaded to the GPU
// for the frame being drawn. `PresentHook` already sees the end of that frame;
// this file sees the middle of it -- the viewport it set, the targets it bound
// and the constant buffers it filled, on Archicad's own render thread, before
// its draws.
//
// ⚠️ IT IS THE SAME VTABLE SWAP AS `PresentHook`, EXTENDED TO A SECOND
// INTERFACE, and that is deliberate rather than convenient. `PresentHook.hpp`
// argues the case in full: a COM interface is reached through a pointer table,
// so redirecting one is a single pointer write behind VirtualProtect --
// reversible exactly, verifiable by comparing pointers, no third-party code and
// no disassembler. An inline detour (MinHook and friends) rewrites the first
// bytes of the function, races any other product that hooked it, and fails by
// crashing rather than misbehaving. Nothing here may resurrect that idea.
//
// ⚠️ THE VTABLE IS SHARED BY EVERY DEVICE CONTEXT IN THE PROCESS, OURS INCLUDED.
// One table per implementation, not one per object -- so patching it puts our
// detour in front of Diligent's context, Archicad's, and any other D3D11 user in
// the process. Every detour compares the object pointer against the nominated
// Archicad context and returns immediately when it does not match. Getting that
// filter wrong does not merely produce bad data: it means recording, and
// eventually acting on, OUR OWN rendering as though it were Archicad's.
//
// ⚠️ THESE ARE FAR HOTTER THAN `Present` -- thousands of calls per frame against
// tens. That is the single biggest risk this file carries, because PLAT-RE118
// already measured that Archicad's frame time is sensitive to work added on this
// thread. So every slot has its OWN atomic enable flag and they can be turned on
// one at a time; the two hottest (`Map`/`Unmap` and `UpdateSubresource`) default
// to OFF. A detour whose slot is disabled is a relaxed atomic load, a compare
// and a tail call to the original.
//
// ⚠️ ARCHICAD'S RENDER THREAD, POSSIBLY MORE THAN ONE. Atomics only, no lock, no
// allocation, and NEVER ACAPI. Everything is recorded into fixed rings and
// tables; reading them out is the main thread's job. The one exception is a COM
// call to size a constant buffer, and it is made at most ONCE per resource
// pointer and cached -- the same rule, and the same reason, as
// `PresentHook::RememberChainWindow`.
//
// ⚠️ UNHOOKING IS THE DANGEROUS HALF. `RemoveContextHook` restores every slot
// and then waits for the in-flight counter to reach zero, exactly as
// `RemovePresentHook` does. Nothing we install may outlive the module.
//
// ⚠️ THE VTABLE INDICES ARE VERIFIED BY CALLING THEM, NOT BY BEING BELIEVED.
// They are fixed by the COM ABI and taken from `d3d11.h`'s own vtable struct,
// but a wrong index writes a function pointer into an unrelated slot and the
// failure is a crash inside the driver with nothing pointing at us. So after the
// swap, and BEFORE Archicad's context is ever nominated, each hooked method is
// called on a THROWAWAY context of our own and the detour is required to have
// seen it. Any slot that does not answer fails the install and the whole table
// is restored. See `InstallContextHook`.
//
// ⚠️ THE TABLE IS READ OFF ARCHICAD'S OWN CONTEXT, NOT OFF A THROWAWAY, AND THE
// FIRST LIVE RUN IS WHY (2026-09-13). That run discovered the vtable from a
// throwaway device created with default flags, patched it, self-tested all
// eleven slots green -- and then recorded ZERO detour entries over 724 Archicad
// frames. Not one, from any context in the process, including our own Diligent
// renderer drawing continuously. The table was real, reachable and simply nobody
// else's: **D3D11 does not have one `ID3D11DeviceContext` implementation.** The
// runtime wraps the immediate context -- a thread-safety layer, a debug layer,
// different concrete classes per feature level -- so a device created with
// different creation flags dispatches through a different vtable entirely.
//
// Two consequences, and neither is optional. The hook takes the table from the
// context it actually intends to record. And the self-test throwaway is created
// with ARCHICAD'S OWN `GetCreationFlags` and `GetFeatureLevel` so that it lands
// on that same table; if it does not, the install REFUSES rather than skipping
// the self-test, because an unproven index is the one failure mode this whole
// arrangement exists to prevent.
//
// ⚠️ WHICH MAKES THE INSTALL DEFERRED, NOT IMMEDIATE. Archicad's context is only
// knowable from its swap chain, which is only identified after ~60 frames have
// gone through the present hook. So arming sets `SetContextHookWanted`, the
// present detour discovers the device and context, and the camera tick installs
// on the first tick where both are known. `ContextHookInstalled` staying false
// for a second or two after arming is expected; staying false for longer has a
// reason, and it is in `GetContextHookStats().lastError`.

#include <cstddef>
#include <cstdint>
#include <string>

struct ID3D11DeviceContext;
struct IDXGISwapChain;

namespace geomsrv {
namespace archviz {
namespace dxgi {

// The discovery set, one enumerator per patched vtable slot. Development only:
// stage 5 chooses the narrow permanent hooks, and this whole set comes out.
enum class ContextSlot : uint32_t {
    RSSetViewports = 0,
    RSSetScissorRects,
    OMSetRenderTargets,
    VSSetConstantBuffers,
    PSSetConstantBuffers,
    GSSetConstantBuffers,
    Map,
    Unmap,
    UpdateSubresource,
    ClearRenderTargetView,
    ClearDepthStencilView,
    // ⚠️ THE DRAW COUNTERS ARE THE "IS THE SCENE EVEN HERE" TEST, added after the
    // 2026-09-13 third run. That run installed cleanly and recorded 53 viewport
    // sets and 46 target binds across 2722 Archicad frames -- about two a second
    // while orbiting at 100 fps, which is not a 3D renderer doing anything. A
    // scene pass issues draws in the hundreds or thousands PER FRAME, so these
    // three separate "the context we hooked is where Archicad draws" from "the
    // context we hooked only presents".
    DrawIndexed,
    Draw,
    DrawIndexedInstanced,
    // ⚠️ WHAT PUTS PIXELS IN A BACK BUFFER NOBODY DRAWS INTO. The fourth run
    // (2026-09-13) found 2981 presents against four draw calls on a device that
    // is NOT D3D11On12 -- so the content arrives another way, and these are the
    // two ordinary ones: a deferred context played back, or a blit from a
    // surface produced elsewhere. See the detours for what each answer implies.
    CopyResource,
    ExecuteCommandList,
    // ⚠️ THE GAP THE SIXTH RUN EXPOSED. That run confirmed the nominated chain IS
    // Archicad's 3D canvas -- window 920810 at exactly the overlay's own
    // 3432x1803 -- and not OpenGL, not D3D12, not deferred, not blitted. Yet the
    // hooked set saw twelve draws and, impossibly, ZERO constant-buffer binds
    // across a whole orbit. No renderer binds no constant buffers. The set was
    // simply incomplete:
    //
    //   * the draw family is bigger than three. `DrawInstanced` and the indirect
    //     variants are how a modern renderer submits, and none were hooked;
    //   * `CopySubresourceRegion` and `ResolveSubresource` put pixels in a target
    //     without drawing, and an MSAA resolve into the back buffer is exactly
    //     what a 3D view does last;
    //   * ⚠️ AND D3D11.1 GAVE THE CONSTANT-BUFFER SETTERS SEPARATE VTABLE SLOTS.
    //     `VSSetConstantBuffers1` is index 119, not 7. A renderer written against
    //     `ID3D11DeviceContext1` never touches the slot we were watching, which
    //     is precisely the zero we could not explain -- and it is the slot stage 3
    //     needs most, because the camera rides in it.
    DrawInstanced,
    DrawAuto,
    DrawIndexedInstancedIndirect,
    DrawInstancedIndirect,
    Dispatch,
    DispatchIndirect,
    CopySubresourceRegion,
    ResolveSubresource,
    VSSetConstantBuffers1,
    PSSetConstantBuffers1,
    UpdateSubresource1,
    Count
};

const char* ContextSlotName (ContextSlot slot);

// MAIN THREAD. Read the eleven slots out of ARCHICAD'S OWN context vtable and
// hand each to `patchprofile::RecordTarget`, WITHOUT patching anything.
//
// ⚠️ IT NEEDS THE CONTEXT, SO IT NEEDS THE MODE ARMED FIRST. The profile has to
// describe the slots actually being patched, and after the 2026-09-13 run those
// can only come from Archicad's own table -- a throwaway's would pin a table
// nobody dispatches through, which is precisely the bug that made the first run
// record nothing. So `Tapioca.ViewerPatchProfile {pin: true}` is a two-pass
// affair on a fresh build: arm `hookdiag` with `gpuState`, let the present hook
// find the context, then pin. The command says so when it cannot.
bool FingerprintContextTargets (std::string& error);

// MAIN THREAD. Fingerprints the target's real vtable, verifies the patch
// profile, patches the discovery set, then self-tests every patched slot against
// a throwaway created with the same device flags. False with `error` filled if
// any step refused, having undone whatever it managed -- a partial install is
// never left behind.
//
// ⚠️ IT REFUSES ON AN UNPINNED OR MISMATCHED BUILD. See `PatchProfile.hpp`: the
// GPU-state path reads Archicad's own buffers and their layout is build-
// specific, so running it on an unverified build is how this ends as a crash in
// somebody else's Archicad. The refusal is the feature.
//
// ⚠️ AND IT LATCHES A FAILURE. The camera tick calls this every tick while the
// mode is armed and the hook is not up; without a latch a refused install would
// re-create a D3D11 device and re-hash two system DLLs sixty times a second.
// `RetryContextHookInstall` clears the latch, and pinning a profile does.
bool InstallContextHook (std::string& error);
void RetryContextHookInstall ();

// MAIN THREAD. Idempotent. Restores every slot and waits for in-flight detour
// calls to drain before returning.
void RemoveContextHook ();

// RENDER THREAD (from the present detour, once per Archicad frame) AND MAIN
// THREAD (from the camera tick, as a backstop). Put back any slot that is no
// longer ours.
//
// ⚠️ IT IS SAFE ON BOTH BECAUSE IT IS IDEMPOTENT AND WRITES POINTER-SIZED
// ALIGNED SLOTS. Two threads racing to restore the same detour write the same
// value; a thread racing the D3D11 runtime writes a whole pointer or none of it,
// never half. The worst outcome is one call reaching the original, which is the
// same outcome as not having repaired yet.
//
// The one interleaving that is genuinely lossy: this function reads a
// re-pointed slot, is preempted, the runtime re-points it again, the other
// caller records the newer value, and then this one resumes and records the
// older one over the top. It is not worth a lock -- a lock is forbidden on the
// render thread, and the window is two instructions wide against an event that
// happens on the order of once a frame -- and it self-corrects at the next
// present, which finds the slot holding our detour and the original stale only
// until the runtime touches it again.
//
// ⚠️ NO ALLOCATION, NO LOGGING, NO ACAPI, because of the thread it is on. That
// is why it does not share `WithWritableVtable`: the helper formats an error
// string, and the error path is the one that would allocate.
//
// ⚠️ THIS IS NOT DEFENSIVE PROGRAMMING; IT IS THE WHOLE REASON THE HOOK WORKS.
// The eighth live run (2026-09-13) reported ZERO of twenty-seven slots still
// holding their detour at the end of a twelve-second orbit -- and that one number
// retracted every conclusion the previous four runs had reached. "Archicad only
// makes four draws", "nothing draws on this context", "the scene must be rendered
// somewhere we cannot see": all of them were an artefact of a hook that recorded
// one or two frames and was then quietly overwritten.
//
// It is overwritten because ⚠️ ARCHICAD'S CONTEXT VTABLE IS INLINE IN THE OBJECT
// -- its address is the object's own address plus eight, not an address in
// d3d11.dll's read-only data -- and D3D11's runtime re-points those entries as
// device state changes. A table in `.rdata` is written once and stays written,
// which is why the same technique on `IDXGISwapChain` has been stable for months;
// a per-object dispatch table is rebuilt by its owner whenever the owner likes.
//
// ⚠️ THE REPAIR TAKES WHATEVER IS THERE NOW AS THE NEW ORIGINAL. If the runtime
// re-pointed a slot to a different implementation -- a fast path, a validation
// path -- then that is the function our detour must forward to from now on.
// Keeping the pointer captured at install time would forward to a stale
// implementation, which is a far worse failure than not hooking at all.
//
// Returns how many slots it had to put back, so the rate is visible rather than
// silently papered over.
uint32_t RepairContextHook ();

bool ContextHookInstalled ();

// MAIN THREAD. Whether the mode wants this hook up. Set at arm, cleared at
// teardown; the present detour uses it to decide whether to discover Archicad's
// device at all, and the camera tick uses it to decide whether to install.
void SetContextHookWanted (bool wanted);
bool ContextHookWanted ();

// Archicad's immediate context and device, as discovered by the present detour,
// or 0 before that has happened.
void* DiscoveredArchicadContext ();
void* DiscoveredArchicadDevice ();

// RENDER THREAD, from the present detour. Learn Archicad's device and immediate
// context from the swap chain it is presenting, ONCE, and remember both.
//
// ⚠️ IT ASKS COM ON THE HOT PATH EXACTLY ONE TIME, EVER. `GetBuffer` /
// `GetDevice` / `GetImmediateContext` are three COM calls inside a Present, on a
// thread we do not own -- the same thing `PresentHook::RememberChainWindow`
// refuses to do per frame, and for the same reason. It is acceptable once
// because the device cannot change without the chain changing, and `hookdraw`
// already caches the pair this way (HostComposite.cpp). Every call after the
// first is an atomic load and a return.
//
// ⚠️ IT IS THE ONLY WAY THE INSTALL CAN REACH ITS TARGET. The hook reads its
// vtable off Archicad's own context, so without this there is nothing to patch;
// gating it on the hook being installed was a deadlock, and gating it on the
// mode WANTING the hook is what breaks that cycle.
void NominateArchicadContextFrom (IDXGISwapChain* swapChain);

// MAIN THREAD. Per-slot gating -- see the frame-cost warning in the header.
void SetContextSlotEnabled (ContextSlot slot, bool enabled);
bool ContextSlotEnabled (ContextSlot slot);

// One recorded call. Fixed size, no strings, no allocation: this is written from
// a detour.
struct ContextEvent {
    uint64_t timestampUs = 0;
    uint32_t slot = 0;      // ContextSlot
    uint32_t a = 0;         // slot-specific; see ContextHook.cpp for the mapping
    uint32_t b = 0;
    uint32_t c = 0;
    uint64_t handle = 0;    // the resource or view the call named, as an integer
};

// MAIN THREAD. Copy out up to `max` events not yet drained, oldest first, and
// mark them consumed. Returns how many were written.
size_t DrainContextEvents (ContextEvent* out, size_t max);

// ⚠️ THE CALL-BASED SELF-TEST IS THE STRONGEST PROOF, NOT THE ONLY ONE, and the
// 2026-09-13 second run is what forced the distinction. Reading the table off
// Archicad's own context fixed the "we patched a table nobody uses" bug -- but it
// also means the self-test throwaway, which can only ever call methods on ITS
// object, no longer necessarily shares that table. Creating it with Archicad's
// own `GetCreationFlags` and `GetFeatureLevel` gets it there sometimes and not
// always, and refusing outright when it does not made the whole rung
// unreachable on this machine.
//
// What is actually load-bearing is that ⚠️ THE INDEX IS FIXED BY THE COM ABI.
// `ID3D11DeviceContext`'s method order is part of the interface contract: any
// correct implementation lays it out identically, which is exactly why
// `PresentHook` has shipped for months on a module check and the ABI alone. The
// call-test is belt-and-braces on top. So the install grades its own confidence
// and says which grade it got, rather than pretending there are only two states.
enum class Proof : uint32_t {
    None = 0,
    AbiAndPin,            // ABI order + every slot in d3d11.dll + distinct + pinned bytes
    SharedImplementation, // ... and Archicad's slots are the SAME functions as a proven table
    ProvenByCall          // ... and the detour was reached by calling each one
};

const char* ProofName (Proof proof);

struct ContextHookStats {
    bool     installed = false;
    bool     wanted = false;          // the mode asked for it
    bool     pinned = false;          // a patch profile verified at install time
    uint64_t discoveredContext = 0;   // 0 until the present detour has found it
    uint64_t archicadContext = 0;     // 0 until nominated
    uint64_t calls = 0;               // detour entries for the nominated context
    // Detour entries filtered out as somebody else's context -- our own Diligent
    // renderer, in practice. ⚠️ COUNTED ONLY FOR SLOTS THAT ARE ON: counting a
    // disabled slot's traffic would make it as expensive as an enabled one, and
    // the whole point of the gating is that a disabled slot is a load, a compare
    // and a tail call.
    uint64_t otherContextCalls = 0;
    uint64_t eventsRecorded = 0;
    uint64_t eventsDropped = 0;       // ring overrun: the frame was busier than it holds
    // Per-slot call counts for the nominated context, indexed by ContextSlot.
    uint64_t perSlot[size_t (ContextSlot::Count)] = {};
    // Microseconds between the first and last call recorded from Archicad's
    // context. See `g_firstCallUs`: a total cannot tell "Archicad draws four
    // times an orbit" from "the hook stopped after one frame", and a span can.
    uint64_t callSpanUs = 0;

    // How many times `RepairContextHook` has had to put a slot back. ⚠️ A LARGE
    // NUMBER HERE IS NORMAL AND IS THE POINT, not a fault: the runtime rewrites
    // this table as a matter of course. A number that is zero while `calls` is
    // also zero means the repair is not running at all.
    uint64_t repairs = 0;

    // ⚠️ HOW MANY OF THE PATCHED SLOTS STILL HOLD OUR DETOUR, re-read from the
    // live table at report time. Anything less than all of them means something
    // put the originals back -- Archicad recreating the context object, or
    // another product hooking the same table -- and that would explain a hook
    // that counted one frame and then went quiet without any of it being
    // Archicad's behaviour.
    uint32_t slotsStillPatched = 0;

    // How well the eleven vtable indices are proven on this install. The COM ABI
    // fixes the order and that is what carries the weight; the call-based
    // self-test is belt-and-braces and is only available when a throwaway device
    // happens to land on the same table. See `Proof` in the .cpp.
    std::string proof;
    // Why the last install refused, when it did. Kept for the diagnostic.
    std::string lastError;
};
ContextHookStats GetContextHookStats ();

// Write the drained events to the nav log as `source=ctx` rows. MAIN THREAD --
// it does file IO, and the detour must never touch a file.
void FlushContextLog ();

// The same, but only once the ring is half full -- called from the camera-sync
// tick for the reason `FlushPresentLogIfFilling` gives: the ring holds seconds
// and a run is minutes, so flushing only at teardown would keep the last gesture
// and silently drop everything before it.
void FlushContextLogIfFilling ();

}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv

#endif
