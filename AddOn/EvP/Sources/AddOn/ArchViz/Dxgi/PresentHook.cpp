// ArchViz/Dxgi/PresentHook -- see the header. Every rule about this file is in
// that header's comments; this is the mechanism.

#include "ArchViz/Dxgi/PresentHook.hpp"

#include "ArchViz/ArchVizLog.hpp"   // ArchVizLog
#include "ArchViz/Dxgi/HookMarker.hpp"
#include "ArchViz/Dxgi/HostComposite.hpp"
#include "ArchViz/NavLog.hpp"

// windows.h defines min/max as macros, which makes every std::min<T> below a
// syntax error rather than an overload problem.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <atomic>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace dxgi {

namespace {

// ---- vtable indices --------------------------------------------------------
// IUnknown 0-2, IDXGIObject 3-6, IDXGIDeviceSubObject 7, then IDXGISwapChain
// from 8. IDXGISwapChain1 continues at 18. These are fixed by the COM ABI --
// they cannot change without breaking every program on the system -- but they
// are still verified against a real interface pointer at install time rather
// than trusted, because a wrong index here writes a function pointer into an
// unrelated slot and the failure is a crash inside dxgi.dll with no clue as to
// why.
constexpr size_t kPresentIndex = 8;
constexpr size_t kResizeBuffersIndex = 13;
constexpr size_t kPresent1Index = 22;

using PresentFn = HRESULT (STDMETHODCALLTYPE*) (IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT (STDMETHODCALLTYPE*) (IDXGISwapChain1*, UINT, UINT,
                                                 const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT (STDMETHODCALLTYPE*) (IDXGISwapChain*, UINT, UINT, UINT,
                                                      DXGI_FORMAT, UINT);

PresentFn       g_originalPresent = nullptr;
Present1Fn      g_originalPresent1 = nullptr;
ResizeBuffersFn g_originalResizeBuffers = nullptr;

void** g_vtable = nullptr;
std::atomic<bool> g_installed {false};

// ⚠️ THE DRAIN COUNTER. Incremented on entry to every detour and decremented on
// exit; Remove restores the pointers and then spins until it reads zero. Without
// it, a Present already inside the detour returns into a DLL that has unloaded.
std::atomic<int32_t> g_inFlight {0};

std::atomic<uint64_t> g_presentCalls {0};
std::atomic<uint64_t> g_present1Calls {0};
std::atomic<uint64_t> g_resizeCalls {0};

// Our overlay's swap chain, so a row can say whose frame it was. See the header.
std::atomic<uint64_t> g_ownSwapChain {0};

// ---- the sample ring -------------------------------------------------------
// Fixed size, overwritten oldest-first, no allocation on the render thread. A
// present every 8 ms fills 4096 slots in about half a minute, which is longer
// than any single matrix cell.
struct PresentSample {
    uint64_t swapChain;
    uint64_t timestampUs;
    uint32_t width;
    uint32_t height;
    uint32_t syncInterval;
};
constexpr size_t kRingSize = 4096;
PresentSample g_ring[kRingSize] = {};
std::atomic<uint64_t> g_ringReserved {0};    // slots handed out
std::atomic<uint64_t> g_ringPublished {0};   // slots fully written

uint64_t MicrosecondsNow ()
{
    LARGE_INTEGER frequency = {};
    LARGE_INTEGER counter = {};
    if (!QueryPerformanceFrequency (&frequency) || frequency.QuadPart == 0 ||
        !QueryPerformanceCounter (&counter))
        return 0;
    return uint64_t (counter.QuadPart * 1000000ll / frequency.QuadPart);
}

// One cached (chain -> HWND) pair per chain seen. Fixed size and lock-free:
// the render thread only ever appends, and the main thread only ever reads
// published entries. Eight is far more surfaces than Archicad has ever shown.
constexpr size_t kWindowCacheSize = 8;
struct ChainWindow {
    std::atomic<uint64_t> chain {0};
    std::atomic<uint64_t> window {0};
};
ChainWindow g_windowCache[kWindowCacheSize];

// RENDER THREAD. Asks GetDesc at most ONCE per chain -- see the header for why
// per-frame is forbidden.
void RememberChainWindow (IDXGISwapChain* swapChain)
{
    const uint64_t key = uint64_t (uintptr_t (swapChain));
    for (ChainWindow& entry : g_windowCache) {
        const uint64_t seen = entry.chain.load (std::memory_order_acquire);
        if (seen == key)
            return;                      // already asked
        if (seen != 0)
            continue;
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (FAILED (swapChain->GetDesc (&desc)))
            return;
        // ⚠️ THE WINDOW IS PUBLISHED BEFORE THE KEY. A reader that saw the key
        // first could read a zero window and cache "this chain has no window"
        // for the rest of the session.
        entry.window.store (uint64_t (uintptr_t (desc.OutputWindow)),
                            std::memory_order_release);
        entry.chain.store (key, std::memory_order_release);
        return;
    }
}

// Called from the render thread. Atomics only -- see the header.
void RecordPresent (IDXGISwapChain* swapChain, UINT syncInterval)
{
    RememberChainWindow (swapChain);
    // ⚠️ NO GetDesc CALL HERE. It is a COM call on the object we are in the
    // middle of a method of, on a thread we do not own, and it can block. The
    // size is recovered when the ring is read out, from the swap chain the main
    // thread already knows about; a diagnostic is not worth deadlocking a render
    // thread for.
    const uint64_t slot = g_ringReserved.fetch_add (1, std::memory_order_relaxed);
    PresentSample& sample = g_ring[slot % kRingSize];
    sample.swapChain = uint64_t (uintptr_t (swapChain));
    sample.timestampUs = MicrosecondsNow ();
    sample.width = 0;
    sample.height = 0;
    sample.syncInterval = syncInterval;
    // ⚠️ PUBLISHED AFTER THE PAYLOAD, NOT BEFORE. One counter served as both
    // "next slot" and "how many are readable", so the main thread could read a
    // slot the render thread was still filling and log a half-written sample --
    // a frame time built from one row's timestamp and another's. Reserve, fill,
    // then publish; the reader only trusts up to `g_ringPublished`.
    g_ringPublished.fetch_add (1, std::memory_order_release);
}

HRESULT STDMETHODCALLTYPE DetourPresent (IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    // ⚠️ DXGI_PRESENT_TEST DISPLAYS NOTHING. It is a probe for occlusion, and
    // applications issue it while minimised or hidden -- counting it inflates
    // the frame count and, worse, injects timestamps that make the frame clock
    // look faster than the screen ever refreshed.
    if ((flags & DXGI_PRESENT_TEST) == 0) {
        g_presentCalls.fetch_add (1, std::memory_order_relaxed);
        RecordPresent (swapChain, syncInterval);
        // ⚠️ BEFORE THE ORIGINAL, NOT AFTER. After Present the back buffer has
        // already gone to the screen -- with a flip-model chain it is not even
        // the same surface any more -- so anything drawn then appears one frame
        // late at best and never at worst. This is the whole reason these live
        // in the detour instead of on a timer.
        //
        // ⚠️ THE MARKER ONLY WHILE THE COMPOSITOR IS NOT READY. Phase 3's
        // squares are the "the hook is alive" signal; once the real overlay is
        // going in they would sit on top of it and hide the thing being judged.
        if (!HostCompositeReady ())
            DrawMarkerIfTarget (swapChain);
        CompositeOverlayIfTarget (swapChain);
    }
    // ⚠️ THE ORIGINAL IS READ INTO A LOCAL BEFORE THE CALL. Remove can null it
    // between the check and the call otherwise, and a null call here takes
    // Archicad's render thread with it.
    const PresentFn original = g_originalPresent;
    const HRESULT hr = (original != nullptr) ? original (swapChain, syncInterval, flags) : S_OK;
    g_inFlight.fetch_sub (1, std::memory_order_release);
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourPresent1 (IDXGISwapChain1* swapChain, UINT syncInterval,
                                          UINT flags, const DXGI_PRESENT_PARAMETERS* parameters)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    if ((flags & DXGI_PRESENT_TEST) == 0) {
        g_present1Calls.fetch_add (1, std::memory_order_relaxed);
        RecordPresent (swapChain, syncInterval);
        if (!HostCompositeReady ())
            DrawMarkerIfTarget (swapChain);
        CompositeOverlayIfTarget (swapChain);
    }
    const Present1Fn original = g_originalPresent1;
    const HRESULT hr = (original != nullptr)
        ? original (swapChain, syncInterval, flags, parameters) : S_OK;
    g_inFlight.fetch_sub (1, std::memory_order_release);
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourResizeBuffers (IDXGISwapChain* swapChain, UINT bufferCount,
                                               UINT width, UINT height, DXGI_FORMAT format,
                                               UINT flags)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    g_resizeCalls.fetch_add (1, std::memory_order_relaxed);
    // ⚠️ NOTHING TO INVALIDATE HERE, and that is by design. ResizeBuffers fails
    // outright if any reference to a back buffer is outstanding, so a marker
    // that cached a render-target view across frames would break every window
    // resize in Archicad -- from inside Archicad's own resize call, where the
    // error surfaces as its window failing to redraw. The marker takes and
    // releases its reference inside one Present instead.
    const ResizeBuffersFn original = g_originalResizeBuffers;
    const HRESULT hr = (original != nullptr)
        ? original (swapChain, bufferCount, width, height, format, flags) : S_OK;
    g_inFlight.fetch_sub (1, std::memory_order_release);
    return hr;
}

// Swap one vtable entry, returning the previous value. `slot` must already be
// writable.
void* SwapVtableEntry (void** vtable, size_t index, void* replacement)
{
    void* previous = vtable[index];
    vtable[index] = replacement;
    return previous;
}

// ⚠️ THE VTABLE LIVES IN READ-ONLY MEMORY. dxgi.dll's .rdata is mapped without
// write access, so the swap needs VirtualProtect around it -- and the ORIGINAL
// protection has to be put back, because leaving a page of a system DLL
// permanently writable is a real hardening regression and some anti-cheat and
// EDR products treat it as tampering.
bool WithWritableVtable (void** vtable, size_t count, bool (*action) (void**), std::string& error)
{
    DWORD previousProtection = 0;
    const SIZE_T bytes = count * sizeof (void*);
    if (!VirtualProtect (vtable, bytes, PAGE_READWRITE, &previousProtection)) {
        error = "VirtualProtect on the DXGI vtable failed with GetLastError " +
                std::to_string (GetLastError ());
        return false;
    }
    const bool ok = action (vtable);
    DWORD ignored = 0;
    VirtualProtect (vtable, bytes, previousProtection, &ignored);
    return ok;
}

bool ApplyDetours (void** vtable)
{
    g_originalPresent = (PresentFn) SwapVtableEntry (vtable, kPresentIndex, &DetourPresent);
    g_originalPresent1 = (Present1Fn) SwapVtableEntry (vtable, kPresent1Index, &DetourPresent1);
    g_originalResizeBuffers = (ResizeBuffersFn) SwapVtableEntry (
        vtable, kResizeBuffersIndex, &DetourResizeBuffers);
    return true;
}

bool RestoreDetours (void** vtable)
{
    if (g_originalPresent != nullptr)
        SwapVtableEntry (vtable, kPresentIndex, (void*) g_originalPresent);
    if (g_originalPresent1 != nullptr)
        SwapVtableEntry (vtable, kPresent1Index, (void*) g_originalPresent1);
    if (g_originalResizeBuffers != nullptr)
        SwapVtableEntry (vtable, kResizeBuffersIndex, (void*) g_originalResizeBuffers);
    return true;
}

// Create a throwaway device and swap chain purely to read the vtable off it.
//
// ⚠️ DISCOVERED, NEVER HARD-CODED. The address of IDXGISwapChain's vtable is not
// a constant -- it depends on which dxgi.dll is loaded and where it landed. The
// only way to know it is to hold a real swap chain and read the pointer out of
// it. The throwaway is released immediately; the vtable it revealed belongs to
// the implementation, not to that object, and stays valid.
bool DiscoverVtable (void*** outVtable, std::string& error)
{
    // A message-only window cannot host a swap chain, so this is an ordinary
    // window that is never shown.
    HWND window = CreateWindowExW (0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 1, 1,
                                   nullptr, nullptr, nullptr, nullptr);
    if (window == nullptr) {
        error = "could not create the throwaway window for DXGI vtable discovery";
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = 1;
    desc.BufferDesc.Height = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain*      swapChain = nullptr;
    ID3D11Device*        device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const HRESULT hr = D3D11CreateDeviceAndSwapChain (
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &desc, &swapChain, &device, nullptr, &context);

    if (FAILED (hr) || swapChain == nullptr) {
        DestroyWindow (window);
        error = "D3D11CreateDeviceAndSwapChain failed (0x" + std::to_string ((unsigned) hr) +
                "); there is no D3D11 swap chain to hook";
        return false;
    }

    *outVtable = *reinterpret_cast<void***> (swapChain);

    // ⚠️ VERIFY THE TABLE BELONGS TO dxgi.dll BEFORE WRITING TO IT. If the
    // indices or the layout were ever wrong, the alternative is patching an
    // arbitrary pointer in someone else's module -- a crash with no diagnosis.
    // A module check is cheap and turns that into a clean refusal.
    HMODULE owningModule = nullptr;
    const bool located = GetModuleHandleExW (
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR> ((*outVtable)[kPresentIndex]), &owningModule) != 0;

    wchar_t modulePath[MAX_PATH] = {};
    if (located && owningModule != nullptr)
        GetModuleFileNameW (owningModule, modulePath, MAX_PATH);

    swapChain->Release ();
    if (context != nullptr)
        context->Release ();
    if (device != nullptr)
        device->Release ();
    DestroyWindow (window);

    std::wstring path (modulePath);
    std::transform (path.begin (), path.end (), path.begin (), ::towlower);
    if (path.find (L"dxgi.dll") == std::wstring::npos) {
        error = "the Present slot does not belong to dxgi.dll; refusing to patch it";
        return false;
    }
    return true;
}

}   // namespace

bool InstallPresentHook (std::string& error)
{
    if (g_installed.load (std::memory_order_acquire))
        return true;

    void** vtable = nullptr;
    if (!DiscoverVtable (&vtable, error)) {
        ArchVizLog ("present hook: " + error);
        return false;
    }

    // One page's worth is plenty and keeps the writable window small; the three
    // indices patched all sit inside the first 23 slots.
    if (!WithWritableVtable (vtable, kPresent1Index + 1, &ApplyDetours, error)) {
        ArchVizLog ("present hook: " + error);
        return false;
    }

    g_vtable = vtable;
    g_ringPublished.store (0, std::memory_order_release);
    g_ringReserved.store (0, std::memory_order_release);
    // ⚠️ THE COUNTERS RESTART WITH THE HOOK. They are cumulative since Install
    // by contract, and Install used to leave the previous session's totals in
    // place -- so a run that saw nothing could inherit a successful run's
    // numbers and read as healthy. Every diagnostic that branches on
    // "presentCalls == 0" was wrong in exactly the case it existed for.
    g_presentCalls.store (0, std::memory_order_relaxed);
    g_present1Calls.store (0, std::memory_order_relaxed);
    g_resizeCalls.store (0, std::memory_order_relaxed);
    for (ChainWindow& entry : g_windowCache) {
        entry.chain.store (0, std::memory_order_relaxed);
        entry.window.store (0, std::memory_order_relaxed);
    }
    g_installed.store (true, std::memory_order_release);
    ArchVizLog ("present hook: installed (DIAGNOSTIC ONLY -- it records when frames go out "
                "and draws nothing)");
    return true;
}

void RemovePresentHook ()
{
    if (!g_installed.load (std::memory_order_acquire))
        return;

    std::string error;
    if (g_vtable != nullptr)
        WithWritableVtable (g_vtable, kPresent1Index + 1, &RestoreDetours, error);
    g_installed.store (false, std::memory_order_release);

    // ⚠️ DRAIN BEFORE RETURNING. The pointers are restored, so no NEW call can
    // enter a detour -- but calls already inside one are still running on a
    // render thread, and this function's caller may be on its way to unloading
    // the DLL. A bounded spin: if a render thread is wedged we would rather log
    // and continue than hang Archicad's shutdown forever.
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (g_inFlight.load (std::memory_order_acquire) <= 0)
            break;
        Sleep (1);
    }
    if (g_inFlight.load (std::memory_order_acquire) > 0)
        ArchVizLog ("present hook: WARNING -- a detour was still in flight after 1s; "
                    "the vtable is restored but unloading now is not safe");

    g_originalPresent = nullptr;
    g_originalPresent1 = nullptr;
    g_originalResizeBuffers = nullptr;
    g_vtable = nullptr;
    ArchVizLog ("present hook: removed");
}

bool PresentHookInstalled ()
{
    return g_installed.load (std::memory_order_acquire);
}

void SetOwnSwapChain (uint64_t swapChain)
{
    g_ownSwapChain.store (swapChain, std::memory_order_release);
}

uint64_t SwapChainWindow (uint64_t swapChain)
{
    for (const ChainWindow& entry : g_windowCache) {
        if (entry.chain.load (std::memory_order_acquire) == swapChain && swapChain != 0)
            return entry.window.load (std::memory_order_acquire);
    }
    return 0;
}

PresentStats GetPresentStats ()
{
    PresentStats stats;
    stats.installed = g_installed.load (std::memory_order_acquire);
    stats.presentCalls = g_presentCalls.load (std::memory_order_relaxed);
    stats.present1Calls = g_present1Calls.load (std::memory_order_relaxed);
    stats.resizeCalls = g_resizeCalls.load (std::memory_order_relaxed);

    // Snapshot the ring, then find the swap chain presenting most often and its
    // frame-to-frame spacing. That chain is Archicad's: the overlay presents
    // only when the camera moves, Archicad presents whenever it redraws.
    const uint64_t writes = g_ringPublished.load (std::memory_order_acquire);
    const size_t count = size_t (std::min<uint64_t> (writes, kRingSize));
    if (count < 2)
        return stats;

    std::vector<PresentSample> samples (g_ring, g_ring + count);
    std::vector<uint64_t> chains;
    for (const PresentSample& sample : samples)
        chains.push_back (sample.swapChain);
    std::sort (chains.begin (), chains.end ());

    // ⚠️ OUR OWN CHAIN IS EXCLUDED. "Busiest" was meant to identify ARCHICAD's
    // presenter, and the overlay renders continuously while Archicad redraws on
    // demand -- so ours usually wins, and the reported "Archicad frame clock"
    // was our own render loop.
    const uint64_t ownChain = g_ownSwapChain.load (std::memory_order_acquire);
    uint64_t bestChain = 0;
    uint64_t bestCount = 0;
    for (size_t i = 0; i < chains.size ();) {
        size_t j = i;
        while (j < chains.size () && chains[j] == chains[i])
            ++j;
        if (chains[i] == ownChain && ownChain != 0) {
            i = j;
            continue;
        }
        if (uint64_t (j - i) > bestCount) {
            bestCount = uint64_t (j - i);
            bestChain = chains[i];
        }
        i = j;
    }
    stats.busiestSwapChain = bestChain;
    stats.busiestFrameCount = bestCount;

    std::vector<uint64_t> timestamps;
    for (const PresentSample& sample : samples) {
        if (sample.swapChain == bestChain && sample.timestampUs != 0)
            timestamps.push_back (sample.timestampUs);
    }
    std::sort (timestamps.begin (), timestamps.end ());
    std::vector<uint64_t> deltas;
    for (size_t i = 1; i < timestamps.size (); ++i)
        deltas.push_back (timestamps[i] - timestamps[i - 1]);
    if (deltas.empty ())
        return stats;
    std::sort (deltas.begin (), deltas.end ());
    stats.medianFrameUs = deltas[deltas.size () / 2];
    stats.p95FrameUs = deltas[std::min (deltas.size () - 1, size_t (deltas.size () * 95 / 100))];
    return stats;
}

void FlushPresentLog ()
{
    const uint64_t writes = g_ringPublished.load (std::memory_order_acquire);
    const size_t count = size_t (std::min<uint64_t> (writes, kRingSize));
    if (count == 0)
        return;

    // ⚠️ THE TWO CLOCKS ARE TIED TOGETHER HERE, ONCE. The ring is stamped with
    // QPC microseconds (the detour is measuring sub-millisecond spacing); every
    // other row in the log is milliseconds since the session header. Anchoring
    // both to "now" and working backwards puts the frames on the shared
    // timeline, which is the whole point -- a frame clock that cannot be lined
    // up against the camera rows answers none of the questions it was recorded
    // for. Done per flush rather than per row so every sample in a batch shares
    // one anchor and their spacing survives exactly.
    const uint64_t nowUs = MicrosecondsNow ();
    const uint64_t nowSessionMs = navlog::SessionNowMs ();

    for (size_t i = 0; i < count; ++i) {
        const PresentSample& sample = g_ring[i];
        if (sample.timestampUs == 0 || sample.timestampUs > nowUs)
            continue;
        const uint64_t agoMs = (nowUs - sample.timestampUs) / 1000ull;
        const uint64_t sessionMs = (nowSessionMs > agoMs) ? (nowSessionMs - agoMs) : 0;
        const bool ours = (sample.swapChain != 0 &&
                           sample.swapChain == g_ownSwapChain.load (std::memory_order_acquire));
        navlog::LogPresent (sessionMs, sample.swapChain, sample.timestampUs,
                            sample.syncInterval, ours);
    }
    g_ringPublished.store (0, std::memory_order_release);
    g_ringReserved.store (0, std::memory_order_release);
}

void FlushPresentLogIfFilling ()
{
    // ⚠️ WITHOUT THIS THE HOOK ONLY EVER REPORTS THE LAST HALF-MINUTE. The ring
    // holds 4096 samples -- about 30 s at 120 Hz -- and a matrix run is several
    // minutes, so flushing only at teardown would silently discard everything
    // before the final cell and produce a confident table covering one gesture.
    // That exact failure has already happened once in this work, with the log's
    // own rotation.
    if (!g_installed.load (std::memory_order_acquire))
        return;
    if (g_ringPublished.load (std::memory_order_acquire) >= kRingSize / 2)
        FlushPresentLog ();
}

}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv
