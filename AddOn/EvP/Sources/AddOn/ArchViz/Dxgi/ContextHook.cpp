// ArchViz/Dxgi/ContextHook -- see the header. Every rule about this file is in
// that header's comments; this is the mechanism.

#include "ArchViz/Dxgi/ContextHook.hpp"

#include "ArchViz/ArchVizLog.hpp"   // ArchVizLog
#include "ArchViz/Dxgi/ContextEventRing.hpp"
#include "ArchViz/Dxgi/ContextHookShared.hpp"
#include "ArchViz/Dxgi/ContextHookSelfTest.hpp"
#include "ArchViz/Dxgi/RenderStateCapture.hpp"
#include "ArchViz/Dxgi/ViewMatrixCandidates.hpp"
#include "ArchViz/NavLog.hpp"
#include "ArchViz/PatchProfile.hpp"

// windows.h defines min/max as macros, which makes every std::min<T> below a
// syntax error rather than an overload problem.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {

// ⚠️ DEFINED HERE, OUTSIDE THE ANONYMOUS NAMESPACE, because the detours in
// ContextHookDetours.cpp write it and this file reads it back. An anonymous
// namespace would give each translation unit its own array: the detours would
// forward through pointers this file never set, and `Remove` would restore a
// table it had never patched.
namespace hookshared {
std::atomic<void*> g_original[size_t (ContextSlot::Count)] = {};
}

using namespace hookshared;

namespace {

void** g_vtable = nullptr;
std::atomic<bool> g_installed {false};
std::atomic<bool> g_wanted {false};
std::atomic<bool> g_pinned {false};
std::string       g_lastError;

// ⚠️ SET WITHOUT AN ADDED REFERENCE, exactly as the filter is. The install only
// ever READS the vtable pointer out of the context and compares object pointers
// afterwards; it never calls through either of these. Holding a reference would
// keep Archicad's device alive past its own teardown, and dropping one later
// would be a release from whichever thread happened to disarm the mode.
std::atomic<uint64_t> g_discoveredContext {0};
std::atomic<uint64_t> g_discoveredDevice {0};

// ⚠️ A REFUSED INSTALL IS LATCHED. The camera tick retries while the mode is
// armed and the hook is down, and a refusal that did not latch would create a
// D3D11 device and hash two system DLLs on every tick -- sixty times a second,
// to reach the same refusal. Cleared by `RetryContextHookInstall`, which pinning
// a profile calls, because pinning is the thing that changes the answer.
std::atomic<bool> g_installRefused {false};
std::atomic<uint64_t> g_repairs {0};

// ⚠️ HOW MANY THREADS ARE INSIDE `RepairContextHook` RIGHT NOW, and it exists for
// exactly one interleaving, which would be silent and fatal. Teardown restores
// the originals; if the present detour ran the repair a microsecond later it
// would see slots that are no longer ours, conclude the runtime had re-pointed
// them, and PUT THE DETOURS BACK -- into a table whose originals teardown is
// about to null. Every Archicad draw call would then reach a detour that
// forwards to nullptr, which the detours correctly decline to call, so Archicad
// would go on running and quietly stop drawing. Nothing would log, and the
// vtable would stay patched for the life of the process.
std::atomic<int32_t> g_repairing {0};

std::atomic<uint32_t> g_proof {uint32_t (Proof::None)};

// ---- the vtable write ------------------------------------------------------
// ⚠️ THE VTABLE LIVES IN READ-ONLY MEMORY, so the swap needs VirtualProtect
// around it -- and the ORIGINAL protection has to be put back, because leaving a
// page of a system DLL permanently writable is a hardening regression and some
// EDR products treat it as tampering. Identical to the present hook's, and
// deliberately not factored into a shared helper: the two differ in the table
// they take and in nothing else, and a shared one would have to be told which
// original pointers to fill in, which is the entire body.
bool WithWritableVtable (void** vtable, size_t count, bool (*action) (void**), std::string& error)
{
    DWORD previousProtection = 0;
    const SIZE_T bytes = count * sizeof (void*);
    if (!VirtualProtect (vtable, bytes, PAGE_READWRITE, &previousProtection)) {
        error = "VirtualProtect on the ID3D11DeviceContext vtable failed with GetLastError " +
                std::to_string (GetLastError ());
        return false;
    }
    const bool ok = action (vtable);
    DWORD ignored = 0;
    VirtualProtect (vtable, bytes, previousProtection, &ignored);
    return ok;
}

void* SwapVtableEntry (void** vtable, size_t index, void* replacement)
{
    void* previous = vtable[index];
    vtable[index] = replacement;
    return previous;
}

bool ApplyDetours (void** vtable)
{
    for (size_t i = 0; i < size_t (ContextSlot::Count); ++i) {
        g_original[i].store (SwapVtableEntry (vtable, kSlotIndex[i], kDetour[i]),
                std::memory_order_relaxed);
    }
    return true;
}

bool RestoreDetours (void** vtable)
{
    for (size_t i = 0; i < size_t (ContextSlot::Count); ++i) {
        void* const original = g_original[i].load (std::memory_order_relaxed);
        if (original != nullptr)
            SwapVtableEntry (vtable, kSlotIndex[i], original);
    }
    return true;
}

// ---- proving the indices ---------------------------------------------------
// Call each patched method on our own throwaway context and require the detour
// to have been reached. See the header: this is what turns eleven hard-coded
// integers into eleven runtime facts.
//
// ⚠️ THE CALLING HALF LIVES IN ContextHookSelfTest, THE VERDICT HERE. That file
// knows how to build a device and how to invoke each method legally; the
// counters it has to move are this file's state, and moving them across would
// split one fact between two translation units.
bool SelfTest (selftest::Throwaway& throwaway, std::string& error)
{
    for (std::atomic<uint64_t>& seen : g_selfTestSeen)
        seen.store (0, std::memory_order_relaxed);
    g_selfTestContext.store (uint64_t (uintptr_t (throwaway.context)), std::memory_order_release);

    selftest::Exercise (throwaway);

    g_selfTestContext.store (0, std::memory_order_release);

    for (size_t i = 0; i < size_t (ContextSlot::Count); ++i) {
        if (g_selfTestSeen[i].load (std::memory_order_relaxed) == 0) {
            error = std::string ("the vtable self-test never reached the detour for ") +
                    ContextSlotName (ContextSlot (i)) + " (slot " +
                    std::to_string (kSlotIndex[i]) +
                    "); the index is wrong for this d3d11.dll and patching it would write "
                    "into an unrelated slot. Nothing is installed";
            return false;
        }
    }
    return true;
}

}   // namespace

const char* ProofName (Proof proof)
{
    switch (proof) {
        case Proof::None:      return "none";
        case Proof::AbiAndPin: return "ABI order + module + distinct slots + pinned bytes";
        case Proof::SharedImplementation: return "the above, and the slots are known functions";
        case Proof::ProvenByCall:         return "the above, and every detour was called";
    }
    return "?";
}

const char* ContextSlotName (ContextSlot slot)
{
    switch (slot) {
        case ContextSlot::RSSetViewports:        return "RSSetViewports";
        case ContextSlot::RSSetScissorRects:     return "RSSetScissorRects";
        case ContextSlot::OMSetRenderTargets:    return "OMSetRenderTargets";
        case ContextSlot::VSSetConstantBuffers:  return "VSSetConstantBuffers";
        case ContextSlot::PSSetConstantBuffers:  return "PSSetConstantBuffers";
        case ContextSlot::GSSetConstantBuffers:  return "GSSetConstantBuffers";
        case ContextSlot::Map:                   return "Map";
        case ContextSlot::Unmap:                 return "Unmap";
        case ContextSlot::UpdateSubresource:     return "UpdateSubresource";
        case ContextSlot::ClearRenderTargetView: return "ClearRenderTargetView";
        case ContextSlot::ClearDepthStencilView: return "ClearDepthStencilView";
        case ContextSlot::DrawIndexed:           return "DrawIndexed";
        case ContextSlot::Draw:                  return "Draw";
        case ContextSlot::DrawIndexedInstanced:  return "DrawIndexedInstanced";
        case ContextSlot::CopyResource:          return "CopyResource";
        case ContextSlot::ExecuteCommandList:    return "ExecuteCommandList";
        case ContextSlot::DrawInstanced:         return "DrawInstanced";
        case ContextSlot::DrawAuto:              return "DrawAuto";
        case ContextSlot::DrawIndexedInstancedIndirect:
            return "DrawIndexedInstancedIndirect";
        case ContextSlot::DrawInstancedIndirect: return "DrawInstancedIndirect";
        case ContextSlot::Dispatch:              return "Dispatch";
        case ContextSlot::DispatchIndirect:      return "DispatchIndirect";
        case ContextSlot::CopySubresourceRegion: return "CopySubresourceRegion";
        case ContextSlot::ResolveSubresource:    return "ResolveSubresource";
        case ContextSlot::VSSetConstantBuffers1: return "VSSetConstantBuffers1";
        case ContextSlot::PSSetConstantBuffers1: return "PSSetConstantBuffers1";
        case ContextSlot::UpdateSubresource1:    return "UpdateSubresource1";
        case ContextSlot::Count:                 return "?";
    }
    return "?";
}

// The vtable Archicad's own immediate context dispatches through, or nullptr
// with `error` filled.
//
// ⚠️ ARCHICAD'S, NOT A THROWAWAY'S. See the header: D3D11 has several
// `ID3D11DeviceContext` implementations and a throwaway made with different
// creation flags gets a different one, which is how the first live run patched a
// perfectly good table that nobody dispatched through and recorded nothing.
static void** ArchicadContextVtable (std::string& error)
{
    ID3D11DeviceContext* target =
        (ID3D11DeviceContext*) DiscoveredArchicadContext ();
    if (target == nullptr) {
        error = "Archicad's immediate context has not been identified yet; it is found "
                "from its swap chain in the present detour and that needs about 60 frames "
                "of Archicad redrawing. Navigate the 3D window";
        return nullptr;
    }

    void** vtable = *reinterpret_cast<void***> (target);

    // ⚠️ EVERY SLOT, NOT JUST ONE, and the checks live in the self-test file
    // because they answer the same question it does: is this table what we think
    // it is. See `ValidateTable` for why they carry more weight than they used to.
    static selftest::SlotDescriptor descriptors[size_t (ContextSlot::Count)] = {};
    for (size_t i = 0; i < size_t (ContextSlot::Count); ++i) {
        descriptors[i].index = kSlotIndex[i];
        descriptors[i].name = ContextSlotName (ContextSlot (i));
    }
    if (!selftest::ValidateTable (vtable, descriptors, size_t (ContextSlot::Count), error)) {
        error = "Archicad's context " + error;
        return nullptr;
    }
    return vtable;
}

bool FingerprintContextTargets (std::string& error)
{
    void** vtable = ArchicadContextVtable (error);
    if (vtable == nullptr)
        return false;

    patchprofile::ClearTargets ();
    patchprofile::RecordModule (L"d3d11.dll");
    patchprofile::RecordModule (L"dxgi.dll");
    for (size_t i = 0; i < size_t (ContextSlot::Count); ++i) {
        const std::string name = std::string ("ID3D11DeviceContext::") +
                                 ContextSlotName (ContextSlot (i));
        patchprofile::RecordTarget (name.c_str (), vtable[kSlotIndex[i]]);
    }
    return true;
}

bool InstallContextHook (std::string& error)
{
    if (g_installed.load (std::memory_order_acquire))
        return true;
    if (g_installRefused.load (std::memory_order_acquire)) {
        error = g_lastError;
        return false;
    }

    ID3D11Device* device = (ID3D11Device*) DiscoveredArchicadDevice ();
    ID3D11DeviceContext* target = (ID3D11DeviceContext*) DiscoveredArchicadContext ();

    // ⚠️ THE PROFILE IS VERIFIED BEFORE ANYTHING IS WRITTEN, and the
    // fingerprinting that feeds it reads the vtable BEFORE the swap. Verifying
    // afterwards would compare our own detours against the pin and pass forever.
    if (!FingerprintContextTargets (error)) {
        // ⚠️ NOT LATCHED. "The context is not identified yet" is the expected
        // answer for the first second of every run, and latching it would mean
        // the hook never went up at all.
        g_lastError = error;
        return false;
    }
    if (!patchprofile::Verify (error)) {
        g_pinned.store (false, std::memory_order_release);
        g_lastError = error;
        g_installRefused.store (true, std::memory_order_release);
        ArchVizLog ("context hook: refused -- " + error);
        return false;
    }
    g_pinned.store (true, std::memory_order_release);

    void** vtable = ArchicadContextVtable (error);
    if (vtable == nullptr || device == nullptr || target == nullptr) {
        g_lastError = error;
        return false;
    }

    // ⚠️ THE THROWAWAY IS BUILT FROM ARCHICAD'S OWN DEVICE DESCRIPTION so that it
    // lands on the same implementation, and therefore the same vtable. This is
    // the whole repair of the 2026-09-13 run.
    selftest::Throwaway throwaway;
    if (!selftest::Create (device->GetCreationFlags (), int (device->GetFeatureLevel ()),
                           throwaway, error)) {
        g_lastError = error;
        g_installRefused.store (true, std::memory_order_release);
        ArchVizLog ("context hook: " + error);
        return false;
    }

    // ⚠️ THE TWO TABLES OFTEN DIFFER, AND THAT IS NOT A REFUSAL ANY MORE. The
    // 2026-09-13 second run hit exactly this: the throwaway was created with
    // Archicad's own creation flags and feature level and STILL landed on a
    // different `ID3D11DeviceContext` implementation, so refusing made the rung
    // unreachable on the one machine it was built for. See `Proof` for what is
    // actually load-bearing. The grade is recorded and reported instead.
    const bool sameTable = (throwaway.vtable == vtable);
    bool sameFunctions = true;
    for (size_t i = 0; i < size_t (ContextSlot::Count) && !sameTable; ++i) {
        if (throwaway.vtable[kSlotIndex[i]] != vtable[kSlotIndex[i]]) {
            sameFunctions = false;
            break;
        }
    }

    // ⚠️ LOGGED WHETHER IT MATCHED OR NOT, because "why do these differ" is the
    // next question somebody will ask and the answer has to be in the file
    // rather than in a debugger. Archicad's device description is the thing that
    // was supposed to make them match.
    {
        char detail[320] = {};
        std::snprintf (detail, sizeof (detail),
                       "context hook: Archicad context %p table %p, throwaway table %p "
                       "(creation flags 0x%08x, feature level 0x%04x) -- %s",
                       (void*) target, (void*) vtable, (void*) throwaway.vtable,
                       unsigned (device->GetCreationFlags ()),
                       unsigned (device->GetFeatureLevel ()),
                       sameTable ? "same table"
                                 : (sameFunctions ? "different table, same functions"
                                                  : "different implementation"));
        ArchVizLog (detail);
    }

    // The filter is cleared before the swap, so the very first call through a
    // patched slot -- which may arrive from Archicad's render thread on the next
    // instruction -- is ignored rather than recorded against a stale nomination.
    g_archicadContext.store (0, std::memory_order_release);
    g_selfTestContext.store (0, std::memory_order_release);

    if (!WithWritableVtable (vtable, kVtableSlots, &ApplyDetours, error)) {
        throwaway.Release ();
        g_lastError = error;
        g_installRefused.store (true, std::memory_order_release);
        ArchVizLog ("context hook: " + error);
        return false;
    }
    g_vtable = vtable;

    Proof proof = sameTable ? Proof::ProvenByCall
                            : (sameFunctions ? Proof::SharedImplementation : Proof::AbiAndPin);

    // ⚠️ THE CALL-TEST ONLY MEANS ANYTHING WHEN THE THROWAWAY SHARES THE TABLE WE
    // JUST PATCHED. On a different table our detours are not in its slots, so
    // calling its methods would reach the originals, see nothing, and report a
    // failure that says the indices are wrong when it only says the objects are
    // different.
    if (sameTable && !SelfTest (throwaway, error)) {
        std::string ignored;
        WithWritableVtable (g_vtable, kVtableSlots, &RestoreDetours, ignored);
        // The drain applies to a failed install too: the swap was live for the
        // length of the self-test, so Archicad's thread may be inside a detour
        // right now.
        for (int attempt = 0; attempt < 1000 && g_inFlight.load (std::memory_order_acquire) > 0;
             ++attempt)
            Sleep (1);
        g_vtable = nullptr;
        throwaway.Release ();
        g_lastError = error;
        g_installRefused.store (true, std::memory_order_release);
        ArchVizLog ("context hook: " + error);
        return false;
    }

    throwaway.Release ();
    g_proof.store (uint32_t (proof), std::memory_order_release);

    // ⚠️ THE COUNTERS RESTART WITH THE HOOK. They are cumulative since Install by
    // contract, and a run that saw nothing must not be able to inherit a
    // previous run's totals and read as healthy -- the present hook shipped that
    // exact bug once and every diagnostic that branches on "calls == 0" was
    // wrong in the case it existed for.
    g_calls.store (0, std::memory_order_relaxed);
    g_otherContextCalls.store (0, std::memory_order_relaxed);
    g_firstCallUs.store (0, std::memory_order_relaxed);
    g_lastCallUs.store (0, std::memory_order_relaxed);
    g_repairs.store (0, std::memory_order_relaxed);
    for (std::atomic<uint64_t>& count : g_perSlot)
        count.store (0, std::memory_order_relaxed);
    eventring::Reset ();
    // ⚠️ THE DEFAULTS ARE APPLIED AT EVERY INSTALL, NOT ONCE. Which slots are on
    // is part of the mode's contract, not a setting that survives it: an arm
    // that inherited the last run's gating would produce a run whose cost and
    // whose coverage nobody could name from the log. `SetContextSlotEnabled` is
    // therefore something a caller does AFTER arming, and the diagnostic reports
    // the live state rather than the intent.
    //
    // ⚠️ THE THREE HOTTEST DEFAULT TO OFF. `Map`/`Unmap` cost an uncached read
    // of write-combined memory and `UpdateSubresource` a copy, on Archicad's
    // render thread, per constant-buffer upload -- and PLAT-RE118 already
    // measured this thread to be sensitive to added work. Stage 3 turns them on
    // deliberately, one at a time, with the frame clock watched.
    for (size_t i = 0; i < size_t (ContextSlot::Count); ++i) {
        const ContextSlot slot = ContextSlot (i);
        const bool hot = (slot == ContextSlot::Map || slot == ContextSlot::Unmap ||
                          slot == ContextSlot::UpdateSubresource);
        g_slotEnabled[i].store (!hot, std::memory_order_relaxed);
    }

    // ⚠️ THE FILTER IS SET LAST, AFTER THE SELF-TEST HAS PROVEN EVERY SLOT. Until
    // this store the detours are installed but record nothing; from here they
    // record Archicad's calls. Setting it earlier would let a mis-indexed slot
    // write garbage from a live frame into the rings, which is the failure the
    // self-test exists to catch before it can happen.
    g_archicadContext.store (uint64_t (uintptr_t (target)), std::memory_order_release);

    g_lastError.clear ();
    g_installed.store (true, std::memory_order_release);
    ArchVizLog ("context hook: installed on Archicad's own context vtable, " +
                std::to_string (size_t (ContextSlot::Count)) + " slots; proof = " +
                ProofName (proof) +
                " (DISCOVERY ONLY -- it records what Archicad tells the GPU and draws "
                "nothing)");
    return true;
}

void RemoveContextHook ()
{
    if (!g_installed.load (std::memory_order_acquire))
        return;

    // ⚠️ THE FILTER GOES FIRST. Clearing it is one atomic store, after which
    // every detour still in the table becomes a compare and a tail call -- so
    // the window between "restoring" and "restored" records nothing rather than
    // recording into rings the main thread is about to reset.
    g_archicadContext.store (0, std::memory_order_release);

    // ⚠️ `installed` IS CLEARED BEFORE THE RESTORE, NOT AFTER, and the order is
    // load-bearing now that a repair exists. It is the flag the repair checks
    // first, so clearing it here is what stops the render thread re-patching the
    // table behind this function's back. Then wait out the repair that may
    // already be past that check: it is a few dozen instructions, so this
    // normally reads zero on the first try.
    g_installed.store (false, std::memory_order_release);
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (g_repairing.load (std::memory_order_acquire) <= 0)
            break;
        Sleep (1);
    }

    std::string error;
    if (g_vtable != nullptr)
        WithWritableVtable (g_vtable, kVtableSlots, &RestoreDetours, error);

    // ⚠️ DRAIN BEFORE RETURNING, exactly as the present hook does: the pointers
    // are restored so no NEW call can enter a detour, but calls already inside
    // one are still running on a render thread and this function's caller may be
    // on its way to unloading the DLL. Bounded, because hanging Archicad's
    // shutdown forever is worse than logging that we could not be sure.
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (g_inFlight.load (std::memory_order_acquire) <= 0)
            break;
        Sleep (1);
    }
    if (g_inFlight.load (std::memory_order_acquire) > 0)
        ArchVizLog ("context hook: WARNING -- a detour was still in flight after 1s; the "
                    "vtable is restored but unloading now is not safe");

    for (std::atomic<void*>& original : g_original)
        original.store (nullptr, std::memory_order_relaxed);
    g_vtable = nullptr;
    g_pinned.store (false, std::memory_order_release);
    g_proof.store (uint32_t (Proof::None), std::memory_order_release);
    ArchVizLog ("context hook: removed");
}

uint32_t RepairContextHook ()
{
    if (!g_installed.load (std::memory_order_acquire) || g_vtable == nullptr)
        return 0;

    // ⚠️ RAISE THE GUARD, THEN CHECK AGAIN. Teardown clears `installed` and then
    // waits for this counter, so a repair that raises the guard before the clear
    // is waited for, and one that raises it after re-reads the flag and leaves.
    // Checking only once, before the guard, would let a repair slip between the
    // clear and the wait -- see `g_repairing` for what that costs.
    g_repairing.fetch_add (1, std::memory_order_acq_rel);
    if (!g_installed.load (std::memory_order_acquire) || g_vtable == nullptr) {
        g_repairing.fetch_sub (1, std::memory_order_release);
        return 0;
    }

    // Cheap pass first: almost every tick will find nothing to do, and taking
    // VirtualProtect on a page of Archicad's heap sixty times a second to change
    // nothing would be a real cost for no reason.
    uint32_t wrong = 0;
    for (size_t i = 0; i < size_t (ContextSlot::Count); ++i) {
        if (g_vtable[kSlotIndex[i]] != kDetour[i])
            ++wrong;
    }
    if (wrong == 0) {
        g_repairing.fetch_sub (1, std::memory_order_release);
        return 0;
    }

    // ⚠️ NOT `WithWritableVtable`, AND THE REASON IS THE THREAD. That helper
    // formats a `std::string` when VirtualProtect fails, and this function runs
    // inside the present detour on Archicad's render thread, where the rule for
    // this whole subsystem is atomics only and no allocation. The failure path
    // is the one that would allocate, so it is the one that must not exist here.
    // There is nothing useful to say about a failure anyway: the next present is
    // fifteen milliseconds away and will try again.
    DWORD previousProtection = 0;
    const SIZE_T bytes = kVtableSlots * sizeof (void*);
    if (!VirtualProtect (g_vtable, bytes, PAGE_READWRITE, &previousProtection)) {
        g_repairing.fetch_sub (1, std::memory_order_release);
        return 0;
    }

    for (size_t i = 0; i < size_t (ContextSlot::Count); ++i) {
        void* const current = g_vtable[kSlotIndex[i]];
        if (current == kDetour[i])
            continue;
        // ⚠️ THE CURRENT VALUE BECOMES THE ORIGINAL, AND IT IS STORED BEFORE THE
        // SLOT IS PATCHED. The moment the detour goes into the table another
        // thread can enter it and read its original; storing the forward pointer
        // second would give that thread the stale one for the width of two
        // instructions, which is a call into an implementation the runtime has
        // moved on from.
        g_original[i].store (current, std::memory_order_relaxed);
        g_vtable[kSlotIndex[i]] = kDetour[i];
    }

    DWORD ignored = 0;
    VirtualProtect (g_vtable, bytes, previousProtection, &ignored);

    g_repairs.fetch_add (wrong, std::memory_order_relaxed);
    g_repairing.fetch_sub (1, std::memory_order_release);
    return wrong;
}

bool ContextHookInstalled ()
{
    return g_installed.load (std::memory_order_acquire);
}

void NominateArchicadContextFrom (IDXGISwapChain* swapChain)
{
    // ⚠️ GATED ON `wanted`, NOT ON `installed`, AND THAT ORDER IS THE WHOLE
    // POINT NOW. The install needs Archicad's context to read its vtable from,
    // so requiring the install first was a deadlock: the hook could never go up
    // because the thing it needed was only discovered once it was up.
    if (swapChain == nullptr || !g_wanted.load (std::memory_order_acquire))
        return;
    if (g_discoveredContext.load (std::memory_order_acquire) != 0)
        return;

    // Every COM pointer below is released on every path. A leak here is a leaked
    // reference on Archicad's own device taken from inside its Present, and the
    // symptom -- `ResizeBuffers` refusing while a reference is outstanding
    // (PLAT-RE150) -- would show up as Archicad failing to redraw on a window
    // resize, with nothing pointing at us.
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED (swapChain->GetBuffer (0, __uuidof (ID3D11Texture2D), (void**) &backBuffer)) ||
        backBuffer == nullptr)
        return;
    ID3D11Device* device = nullptr;
    backBuffer->GetDevice (&device);
    ID3D11DeviceContext* context = nullptr;
    if (device != nullptr)
        device->GetImmediateContext (&context);

    if (context != nullptr && device != nullptr) {
        // ⚠️ THE POINTERS ARE KEPT, THE REFERENCES ARE NOT -- see the note at the
        // declarations. `HostComposite` makes the same trade for the same reason.
        //
        // ⚠️ THE DEVICE IS PUBLISHED BEFORE THE CONTEXT. The main thread treats a
        // non-zero context as "both are ready" and immediately asks the device
        // for its creation flags; the other order lets it read a zero device.
        g_discoveredDevice.store (uint64_t (uintptr_t (device)), std::memory_order_release);
        g_discoveredContext.store (uint64_t (uintptr_t (context)), std::memory_order_release);
    }
    if (context != nullptr)
        context->Release ();
    if (device != nullptr)
        device->Release ();
    backBuffer->Release ();
}

void SetContextHookWanted (bool wanted)
{
    g_wanted.store (wanted, std::memory_order_release);
    if (!wanted) {
        // The discovery is per-arm: a chain identified for a previous run may
        // belong to a window that has since closed.
        g_discoveredContext.store (0, std::memory_order_release);
        g_discoveredDevice.store (0, std::memory_order_release);
        g_installRefused.store (false, std::memory_order_release);
    }
}

bool ContextHookWanted ()
{
    return g_wanted.load (std::memory_order_acquire);
}

void* DiscoveredArchicadContext ()
{
    return (void*) (uintptr_t) g_discoveredContext.load (std::memory_order_acquire);
}

void* DiscoveredArchicadDevice ()
{
    return (void*) (uintptr_t) g_discoveredDevice.load (std::memory_order_acquire);
}

void RetryContextHookInstall ()
{
    g_installRefused.store (false, std::memory_order_release);
}

void SetContextSlotEnabled (ContextSlot slot, bool enabled)
{
    if (slot < ContextSlot::Count)
        g_slotEnabled[size_t (slot)].store (enabled, std::memory_order_release);
}

bool ContextSlotEnabled (ContextSlot slot)
{
    return slot < ContextSlot::Count &&
           g_slotEnabled[size_t (slot)].load (std::memory_order_acquire);
}

size_t DrainContextEvents (ContextEvent* out, size_t max)
{
    return eventring::Drain (out, max);
}

ContextHookStats GetContextHookStats ()
{
    ContextHookStats stats;
    stats.installed = g_installed.load (std::memory_order_acquire);
    stats.wanted = g_wanted.load (std::memory_order_acquire);
    stats.pinned = g_pinned.load (std::memory_order_acquire);
    stats.discoveredContext = g_discoveredContext.load (std::memory_order_acquire);
    stats.archicadContext = g_archicadContext.load (std::memory_order_acquire);
    stats.calls = g_calls.load (std::memory_order_relaxed);
    stats.otherContextCalls = g_otherContextCalls.load (std::memory_order_relaxed);
    stats.eventsRecorded = eventring::Recorded ();
    stats.eventsDropped = eventring::Dropped ();
    for (size_t i = 0; i < size_t (ContextSlot::Count); ++i)
        stats.perSlot[i] = g_perSlot[i].load (std::memory_order_relaxed);
    const uint64_t first = g_firstCallUs.load (std::memory_order_relaxed);
    const uint64_t last = g_lastCallUs.load (std::memory_order_relaxed);
    stats.callSpanUs = (last > first) ? (last - first) : 0;

    // ⚠️ REPAIR FIRST, THEN COUNT, AND THAT CHANGES WHAT THE NUMBER MEANS.
    // Counting without repairing reports how many slots the runtime happened to
    // have re-pointed at the instant of the read -- a snapshot of a race, not a
    // health measure. The tenth live run read 11 of 27 that way and the report
    // declared itself void, while the same run had recorded 301567 calls over a
    // 27-second span through a hook that was plainly working: Archicad redraws
    // on demand, the user had stopped orbiting, and no present had run to repair
    // the table since the runtime last touched it.
    //
    // After a repair the count answers the question that actually matters --
    // are there slots the repair CANNOT restore -- and anything below the full
    // set is then a real fault rather than a timing artefact. The rate at which
    // the runtime unpatches us is not lost; it is `repairs`.
    RepairContextHook ();

    // ⚠️ RE-READ FROM THE LIVE TABLE, not from what we remember writing. The
    // whole point is to catch the case where the table changed under us.
    if (g_vtable != nullptr && g_installed.load (std::memory_order_acquire)) {
        for (size_t i = 0; i < size_t (ContextSlot::Count); ++i) {
            if (g_vtable[kSlotIndex[i]] == kDetour[i])
                ++stats.slotsStillPatched;
        }
    }

    stats.repairs = g_repairs.load (std::memory_order_relaxed);
    stats.proof = ProofName (Proof (g_proof.load (std::memory_order_acquire)));
    stats.lastError = g_lastError;
    return stats;
}

void FlushContextLog ()
{
    eventring::Flush ();
}

void FlushContextLogIfFilling ()
{
    if (!g_installed.load (std::memory_order_acquire))
        return;
    if (eventring::HalfFull ())
        eventring::Flush ();
}

}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv
