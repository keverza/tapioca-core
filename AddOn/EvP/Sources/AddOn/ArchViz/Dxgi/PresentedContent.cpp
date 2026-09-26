// ArchViz/Dxgi/PresentedContent -- which composited image each Present shows, from pixels.
// Bound by private/docs/architecture/diligent/OVERLAY-INVARIANTS.md: observation only,
// GPU copies under ScopedInjectionGuard, DO_NOT_WAIT readback, no locks, and no heap
// on the Present path -- the frame memory is allocated on the main thread when the
// window opens. PRESENT THREAD for OnPresentBegin/OnPresentForward and all they call.

#include "ArchViz/Dxgi/PresentedContent.hpp"

#include "ArchViz/Dxgi/ContextHook.hpp"
#include "ArchViz/Dxgi/ContextStateTracker.hpp"
#include "ArchViz/Dxgi/SceneCameraPairing.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cstring>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace presentedcontent {

namespace {

constexpr uint32_t kSampleRows = uint32_t (2 * kRows); // BEFORE rows, then AFTER rows
static_assert (kHookSlots == size_t (ContextSlot::Count), "per-slot repair counts must cover every hooked slot");
static_assert (kRelations == size_t (Relation::Unidentified) + 1, "one coverage row per Relation");

enum class SlotState : uint32_t { Free = 0, Sampling = 1, Submitted = 2 };
enum class FrameState : uint32_t { Empty = 0, Submitted = 1, Ready = 2, Written = 3 };

struct Hashes {
    uint64_t row[kRows] = {};
};

bool Equal (const Hashes& a, const Hashes& b)
{
    for (size_t r = 0; r < kRows; ++r)
        if (a.row[r] != b.row[r])
            return false;
    return true;
}

// One Present on its way to the CPU. Present thread only; Reset runs after the drain.
struct Slot {
    SlotState state = SlotState::Free;
    ID3D11Texture2D* staging = nullptr; // width x kSampleRows
    uint64_t presentSerial = 0;
    int64_t qpc = 0;
    uint64_t handoffSerial = 0;
    uint64_t imageGeneration = 0;
    uint64_t cameraImageGeneration = 0;
    bool cameraBound = false;
    int frame = -1; // the kept frame this Present fills, or -1
    // What the hooks saw since the previous Present (Stage 76).
    bool coverageKnown = false;
    uint64_t calls = 0;
    uint64_t draws = 0;
    uint64_t repairs = 0;
};

// What processing remembers about a Present, for the two that follow it.
struct Previous {
    bool valid = false;
    uint64_t presentSerial = 0;
    int64_t qpc = 0;
    uint64_t handoffSerial = 0;
    Hashes after;
    bool displayedKnown = false;
    uint64_t displayed = 0;
    int bucket = -1; // true-delta bucket; -1 unknown
};

std::atomic<bool> g_enabled { false };

// Present thread.
Slot g_slots[kSlots];
ID3D11Texture2D* g_frameStaging[kFrames] = {};       // AFTER our drawing
ID3D11Texture2D* g_frameBeforeStaging[kFrames] = {}; // BEFORE it: Archicad's pixels alone
bool g_frameHasBefore[kFrames] = {};
uint32_t g_targetWidth = 0;
uint32_t g_targetHeight = 0;
uint32_t g_targetFormat = 0;
bool g_createFailed = false;
uint64_t g_presentSerial = 0;
int g_sampling = -1;
Previous g_previous1;
Previous g_previous2;
bool g_firstRepeatPending = false;
int64_t g_firstFrameQpc = 0;
int64_t g_qpcFrequency = 0;
bool g_haveHookBaseline = false;
uint64_t g_lastCalls = 0;
uint64_t g_lastDraws = 0;
uint64_t g_lastRepairs = 0;

// Main thread, at enable: where the per-slot repair counts stood when the window opened.
uint64_t g_slotRepairBaseline[kHookSlots] = {};

// ⚠️ ALLOCATED ON THE MAIN THREAD WHEN THE WINDOW OPENS, freed by Reset after the
// drain. The Present thread only copies into it; a frame is published by the
// release store of Ready and read on the main thread only after the drain.
std::vector<uint8_t> g_framePixels[kFrames];
std::vector<uint8_t> g_frameBeforePixels[kFrames];
FrameInfo g_frameInfo[kFrames];
std::atomic<uint32_t> g_frameState[kFrames];

std::atomic<uint64_t> g_presentsSampled { 0 };
std::atomic<uint64_t> g_presentsProcessed { 0 };
std::atomic<uint64_t> g_slotsBusy { 0 };
std::atomic<uint64_t> g_readbacksPending { 0 };
std::atomic<uint64_t> g_readbackFailures { 0 };
std::atomic<uint64_t> g_createFailures { 0 };
std::atomic<uint64_t> g_abandoned { 0 };
std::atomic<uint64_t> g_chainBreaks { 0 };
std::atomic<uint64_t> g_targetChanges { 0 };
std::atomic<uint64_t> g_composited { 0 };
std::atomic<uint64_t> g_compositedChanged { 0 };
std::atomic<uint64_t> g_repeats { 0 };
std::atomic<uint64_t> g_repeatSame { 0 };
std::atomic<uint64_t> g_repeatOther { 0 };
std::atomic<uint64_t> g_repeatUndecidable { 0 };
std::atomic<uint64_t> g_repeatUnidentified { 0 };
std::atomic<uint64_t> g_firstRepeats { 0 };
std::atomic<uint64_t> g_firstRepeatSame { 0 };
std::atomic<uint64_t> g_firstRepeatOther { 0 };
std::atomic<uint64_t> g_firstRepeatUnidentified { 0 };
std::atomic<uint64_t> g_overlayBound { 0 };
std::atomic<uint64_t> g_trueDelta[kDeltaBuckets];
std::atomic<uint64_t> g_trueDeltaUnknown { 0 };
std::atomic<double> g_screenSeconds[kDeltaBuckets];
std::atomic<double> g_screenSecondsUnknown { 0.0 };
std::atomic<uint64_t> g_bookkeepingDisagrees { 0 };

struct CoverageCounters {
    std::atomic<uint64_t> presents { 0 };
    std::atomic<uint64_t> calls { 0 };
    std::atomic<uint64_t> draws { 0 };
    std::atomic<uint64_t> noDraws { 0 };
    std::atomic<uint64_t> repairs { 0 };
};
CoverageCounters g_coverage[kRelations];
std::atomic<uint64_t> g_presentsWithRepairAfterCalls { 0 };

// Single writer, so a load and a store is an add.
void Accumulate (std::atomic<double>& target, double value)
{
    target.store (target.load (std::memory_order_relaxed) + value, std::memory_order_relaxed);
}

void Bump (std::atomic<uint64_t>& counter)
{
    counter.fetch_add (1, std::memory_order_relaxed);
}

uint64_t HashRow (const uint8_t* bytes, size_t length)
{
    uint64_t hash = 1469598103934665603ull; // FNV-1a 64
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

uint32_t SampleRowY (uint32_t index)
{
    return uint32_t ((uint64_t (g_targetHeight) * (index + 1)) / (kRows + 1)); // H/4, H/2, 3H/4
}

void FrameCrop (uint32_t& x, uint32_t& y, uint32_t& width, uint32_t& height)
{
    width = g_targetWidth < kFrameWidth ? g_targetWidth : kFrameWidth;
    height = g_targetHeight < kFrameHeight ? g_targetHeight : kFrameHeight;
    x = (g_targetWidth - width) / 2;
    y = (g_targetHeight - height) / 2;
}

void ReleaseStaging ()
{
    for (Slot& slot : g_slots) {
        if (slot.staging != nullptr) {
            slot.staging->Release ();
            slot.staging = nullptr;
        }
        slot.state = SlotState::Free;
    }
    for (size_t f = 0; f < kFrames; ++f) {
        if (g_frameStaging[f] != nullptr) {
            g_frameStaging[f]->Release ();
            g_frameStaging[f] = nullptr;
        }
        if (g_frameBeforeStaging[f] != nullptr) {
            g_frameBeforeStaging[f]->Release ();
            g_frameBeforeStaging[f] = nullptr;
        }
        g_frameHasBefore[f] = false;
        // A copy in flight into a released texture is gone; never map it.
        if (g_frameState[f].load (std::memory_order_relaxed) == uint32_t (FrameState::Submitted))
            g_frameState[f].store (uint32_t (FrameState::Empty), std::memory_order_relaxed);
    }
}

// PRESENT THREAD. (Re)create the staging ring for this target. A change of size
// or format mid-window drops everything in flight and restarts the history.
bool EnsureStaging (ID3D11Device* device, const D3D11_TEXTURE2D_DESC& target)
{
    if (g_createFailed)
        return false;
    if (g_slots[0].staging != nullptr && target.Width == g_targetWidth && target.Height == g_targetHeight &&
        uint32_t (target.Format) == g_targetFormat)
        return true;
    if (g_slots[0].staging != nullptr) {
        Bump (g_targetChanges);
        g_previous1 = Previous {};
        g_previous2 = Previous {};
        g_sampling = -1;
    }
    ReleaseStaging ();
    g_targetWidth = target.Width;
    g_targetHeight = target.Height;
    g_targetFormat = uint32_t (target.Format);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = target.Width;
    desc.Height = kSampleRows;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = target.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bool ok = true;
    for (Slot& slot : g_slots)
        ok = ok && SUCCEEDED (device->CreateTexture2D (&desc, nullptr, &slot.staging));
    uint32_t x = 0, y = 0;
    FrameCrop (x, y, desc.Width, desc.Height);
    for (ID3D11Texture2D*& staging : g_frameStaging)
        ok = ok && SUCCEEDED (device->CreateTexture2D (&desc, nullptr, &staging));
    for (ID3D11Texture2D*& staging : g_frameBeforeStaging)
        ok = ok && SUCCEEDED (device->CreateTexture2D (&desc, nullptr, &staging));
    if (!ok) {
        ReleaseStaging ();
        g_createFailed = true;
        Bump (g_createFailures);
        return false;
    }
    return true;
}

// The records are classified strictly in Present order: "the previous Present"
// must mean the previous Present, or SAME and OTHER lose their meaning.
void Process (const Slot& slot, const Hashes& before, const Hashes& after)
{
    Bump (g_presentsProcessed);
    const bool contiguous = g_previous1.valid && slot.presentSerial == g_previous1.presentSerial + 1;
    if (!contiguous) {
        if (g_previous1.valid)
            Bump (g_chainBreaks);
        g_previous1 = Previous {};
        g_previous2 = Previous {};
        g_firstRepeatPending = false;
    }
    // How long the previous Present stayed the newest one presented.
    if (contiguous && g_qpcFrequency > 0) {
        const double seconds = double (slot.qpc - g_previous1.qpc) / double (g_qpcFrequency);
        if (g_previous1.bucket >= 0)
            Accumulate (g_screenSeconds[g_previous1.bucket], seconds);
        else
            Accumulate (g_screenSecondsUnknown, seconds);
    }

    Previous current;
    current.valid = true;
    current.presentSerial = slot.presentSerial;
    current.qpc = slot.qpc;
    current.handoffSerial = slot.handoffSerial;
    current.after = after;
    Relation relation = Relation::Unknown;
    if (contiguous) {
        const bool composited = slot.handoffSerial != 0 && slot.handoffSerial != g_previous1.handoffSerial;
        if (composited) {
            Bump (g_composited);
            if (!Equal (before, g_previous1.after))
                Bump (g_compositedChanged);
            relation = Relation::Composited;
            current.displayedKnown = slot.imageGeneration != 0;
            current.displayed = slot.imageGeneration;
            g_firstRepeatPending = true;
        }
        else {
            Bump (g_repeats);
            const bool twoBack = g_previous2.valid && g_previous2.presentSerial + 1 == g_previous1.presentSerial;
            const bool undecidable = twoBack && Equal (g_previous1.after, g_previous2.after);
            if (undecidable) {
                Bump (g_repeatUndecidable);
                relation = Relation::Undecidable;
                current.displayedKnown = g_previous1.displayedKnown && g_previous2.displayedKnown &&
                                         g_previous1.displayed == g_previous2.displayed;
                current.displayed = g_previous1.displayed;
            }
            else if (Equal (before, g_previous1.after)) {
                Bump (g_repeatSame);
                relation = Relation::SameBuffer;
                current.displayedKnown = g_previous1.displayedKnown;
                current.displayed = g_previous1.displayed;
            }
            else if (twoBack && Equal (before, g_previous2.after)) {
                Bump (g_repeatOther);
                relation = Relation::OtherBuffer;
                current.displayedKnown = g_previous2.displayedKnown;
                current.displayed = g_previous2.displayed;
            }
            else {
                Bump (g_repeatUnidentified);
                relation = Relation::Unidentified;
            }
            if (g_firstRepeatPending && !undecidable) {
                Bump (g_firstRepeats);
                if (relation == Relation::SameBuffer)
                    Bump (g_firstRepeatSame);
                else if (relation == Relation::OtherBuffer)
                    Bump (g_firstRepeatOther);
                else
                    Bump (g_firstRepeatUnidentified);
                g_firstRepeatPending = false;
            }
        }
    }

    if (slot.cameraBound)
        Bump (g_overlayBound);
    if (slot.cameraBound && current.displayedKnown) {
        const int64_t delta = int64_t (slot.cameraImageGeneration) - int64_t (current.displayed);
        current.bucket = delta <= -2 ? 0 : delta >= 2 ? 4 : int (delta + 2);
        Bump (g_trueDelta[current.bucket]);
        if (slot.cameraImageGeneration == slot.imageGeneration && delta != 0)
            Bump (g_bookkeepingDisagrees);
    }
    else {
        Bump (g_trueDeltaUnknown);
    }

    if (slot.coverageKnown) {
        CoverageCounters& coverage = g_coverage[size_t (relation)];
        Bump (coverage.presents);
        coverage.calls.fetch_add (slot.calls, std::memory_order_relaxed);
        coverage.draws.fetch_add (slot.draws, std::memory_order_relaxed);
        coverage.repairs.fetch_add (slot.repairs, std::memory_order_relaxed);
        if (slot.draws == 0)
            Bump (coverage.noDraws);
    }

    if (slot.frame >= 0) {
        FrameInfo& info = g_frameInfo[slot.frame];
        info.relation = relation;
        info.displayedKnown = current.displayedKnown;
        info.displayed = current.displayed;
    }
    g_previous2 = g_previous1;
    g_previous1 = current;
}

// PRESENT THREAD. One kept crop into its pixel memory; false while the GPU is
// still on it. A crop that cannot be mapped at all is counted and left as is.
bool ReadCrop (ID3D11DeviceContext* context, ID3D11Texture2D* staging, const FrameInfo& info,
               std::vector<uint8_t>& pixels)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const HRESULT hr = context->Map (staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
        return false;
    if (FAILED (hr) || mapped.pData == nullptr) {
        Bump (g_readbackFailures);
        return true; // nothing to wait for; the frame is published with what it has
    }
    const size_t rowBytes = size_t (info.width) * 4u;
    if (pixels.size () >= rowBytes * info.height) {
        for (uint32_t y = 0; y < info.height; ++y)
            std::memcpy (pixels.data () + y * rowBytes,
                         static_cast<const uint8_t*> (mapped.pData) + y * mapped.RowPitch, rowBytes);
    }
    context->Unmap (staging, 0);
    return true;
}

// PRESENT THREAD. Read back finished Presents, oldest first, and any kept frame
// whose copy has finished. Never waits.
void ProcessReady (ID3D11DeviceContext* context)
{
    for (int budget = 0; budget < 4; ++budget) {
        int oldest = -1;
        for (int i = 0; i < int (kSlots); ++i) {
            if (g_slots[i].state != SlotState::Submitted)
                continue;
            if (oldest < 0 || g_slots[i].presentSerial < g_slots[oldest].presentSerial)
                oldest = i;
        }
        if (oldest < 0)
            break;
        Slot& slot = g_slots[oldest];
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT hr = context->Map (slot.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            Bump (g_readbacksPending);
            break;
        }
        if (FAILED (hr) || mapped.pData == nullptr) {
            Bump (g_readbackFailures);
            slot.state = SlotState::Free; // the serial gap restarts the history
            continue;
        }
        Hashes before;
        Hashes after;
        const size_t rowBytes = size_t (g_targetWidth) * 4u;
        for (size_t r = 0; r < kRows; ++r) {
            const uint8_t* base = static_cast<const uint8_t*> (mapped.pData);
            before.row[r] = HashRow (base + r * mapped.RowPitch, rowBytes);
            after.row[r] = HashRow (base + (r + kRows) * mapped.RowPitch, rowBytes);
        }
        context->Unmap (slot.staging, 0);
        Process (slot, before, after);
        slot.state = SlotState::Free;
    }

    for (size_t f = 0; f < kFrames; ++f) {
        if (g_frameState[f].load (std::memory_order_relaxed) != uint32_t (FrameState::Submitted) ||
            g_frameStaging[f] == nullptr)
            continue;
        const FrameInfo& info = g_frameInfo[f];
        // Both crops of one Present, or neither yet: the BEFORE copy went out
        // first, so an AFTER that has finished almost always finds it finished.
        if (!ReadCrop (context, g_frameStaging[f], info, g_framePixels[f]))
            continue;
        if (g_frameHasBefore[f] && !ReadCrop (context, g_frameBeforeStaging[f], info, g_frameBeforePixels[f]))
            continue;
        g_frameState[f].store (uint32_t (FrameState::Ready), std::memory_order_release);
    }
}

void CopyRows (ID3D11DeviceContext* context, ID3D11Texture2D* backBuffer, ID3D11Texture2D* staging, uint32_t firstRow)
{
    for (uint32_t r = 0; r < kRows; ++r) {
        const uint32_t y = SampleRowY (r);
        D3D11_BOX box = {};
        box.left = 0;
        box.right = g_targetWidth;
        box.top = y;
        box.bottom = y + 1;
        box.front = 0;
        box.back = 1;
        context->CopySubresourceRegion (staging, 0, 0, firstRow + r, 0, backBuffer, 0, &box);
    }
}

// The transient objects one Present needs: never held past the call (§11).
struct PresentTarget {
    ID3D11Texture2D* backBuffer = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D11_TEXTURE2D_DESC desc = {};

    explicit PresentTarget (IDXGISwapChain* swapChain)
    {
        if (FAILED (swapChain->GetBuffer (0, __uuidof (ID3D11Texture2D), (void**) &backBuffer)) ||
            backBuffer == nullptr)
            return;
        backBuffer->GetDesc (&desc);
        backBuffer->GetDevice (&device);
        if (device != nullptr)
            device->GetImmediateContext (&context);
    }
    ~PresentTarget ()
    {
        if (context != nullptr)
            context->Release ();
        if (device != nullptr)
            device->Release ();
        if (backBuffer != nullptr)
            backBuffer->Release ();
    }
    PresentTarget (const PresentTarget&) = delete;
    PresentTarget& operator= (const PresentTarget&) = delete;
    bool Valid () const
    {
        return backBuffer != nullptr && device != nullptr && context != nullptr;
    }
};

bool WriteBmp (const std::wstring& path, const uint8_t* pixels, uint32_t width, uint32_t height)
{
    BITMAPFILEHEADER file = {};
    BITMAPINFOHEADER info = {};
    const DWORD imageBytes = DWORD (width) * DWORD (height) * 4u;
    file.bfType = 0x4D42; // "BM"
    file.bfOffBits = sizeof (BITMAPFILEHEADER) + sizeof (BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + imageBytes;
    info.biSize = sizeof (BITMAPINFOHEADER);
    info.biWidth = LONG (width);
    info.biHeight = -LONG (height); // top-down, as the rows were copied
    info.biPlanes = 1;
    info.biBitCount = 32; // B8G8R8A8 is already BMP's byte order
    info.biCompression = BI_RGB;
    info.biSizeImage = imageBytes;
    const HANDLE handle =
        ::CreateFileW (path.c_str (), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    bool ok = ::WriteFile (handle, &file, sizeof (file), &written, nullptr) != 0 && written == sizeof (file);
    ok = ok && ::WriteFile (handle, &info, sizeof (info), &written, nullptr) != 0 && written == sizeof (info);
    ok = ok && ::WriteFile (handle, pixels, imageBytes, &written, nullptr) != 0 && written == imageBytes;
    ::CloseHandle (handle);
    return ok;
}

std::string Utf8 (const std::wstring& text)
{
    if (text.empty ())
        return std::string ();
    const int bytes =
        ::WideCharToMultiByte (CP_UTF8, 0, text.c_str (), int (text.size ()), nullptr, 0, nullptr, nullptr);
    std::string out (size_t (bytes > 0 ? bytes : 0), '\0');
    if (bytes > 0)
        ::WideCharToMultiByte (CP_UTF8, 0, text.c_str (), int (text.size ()), &out[0], bytes, nullptr, nullptr);
    return out;
}

// %LOCALAPPDATA%\Tapioca\logs\frames, created if missing. Win32 only: the logs
// folder exists by the time a diagnostic runs, so no data-root migration applies.
std::wstring FramesDirectory ()
{
    wchar_t localAppData[MAX_PATH] = {};
    const DWORD length = ::GetEnvironmentVariableW (L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return std::wstring ();
    std::wstring directory (localAppData);
    directory += L"\\Tapioca";
    ::CreateDirectoryW (directory.c_str (), nullptr);
    directory += L"\\logs";
    ::CreateDirectoryW (directory.c_str (), nullptr);
    directory += L"\\frames";
    ::CreateDirectoryW (directory.c_str (), nullptr);
    return directory;
}

} // namespace

void SetEnabled (bool enabled, int repairAfterCallsForced)
{
    // ⚠️ ONLY ON THE DISABLED -> ENABLED EDGE. Enabling an enabled window would
    // reallocate memory a Present may be copying into at this moment.
    if (enabled && !Enabled ()) {
        // The Present path must not allocate, so the frame memory exists before
        // the first Present of the window can see `enabled`. A window opened
        // without a Reset still starts with no frames of the previous one.
        for (size_t f = 0; f < kFrames; ++f) {
            g_framePixels[f].assign (size_t (kFrameWidth) * kFrameHeight * 4u, 0);
            g_frameBeforePixels[f].assign (size_t (kFrameWidth) * kFrameHeight * 4u, 0);
            g_frameHasBefore[f] = false;
            g_frameInfo[f] = FrameInfo {};
            g_frameState[f].store (uint32_t (FrameState::Empty), std::memory_order_relaxed);
        }
        g_firstFrameQpc = 0;
        for (size_t i = 0; i < kHookSlots; ++i)
            g_slotRepairBaseline[i] = ContextSlotRepairs (ContextSlot (i));
    }
    // Forced only while this window is open; disabling always restores the default.
    SetRepairAfterCallsForced (enabled ? repairAfterCallsForced : -1);
    g_enabled.store (enabled, std::memory_order_release);
}

bool Enabled ()
{
    return g_enabled.load (std::memory_order_acquire);
}

void Reset ()
{
    // After the drain: nothing on the Present thread is inside this module.
    for (Slot& slot : g_slots) {
        slot.state = SlotState::Free;
        slot.presentSerial = 0;
        slot.frame = -1;
    }
    g_presentSerial = 0;
    g_sampling = -1;
    g_previous1 = Previous {};
    g_previous2 = Previous {};
    g_firstRepeatPending = false;
    g_firstFrameQpc = 0;
    for (size_t f = 0; f < kFrames; ++f) {
        g_frameInfo[f] = FrameInfo {};
        g_frameState[f].store (uint32_t (FrameState::Empty), std::memory_order_relaxed);
        std::vector<uint8_t> ().swap (g_framePixels[f]);
        std::vector<uint8_t> ().swap (g_frameBeforePixels[f]);
        g_frameHasBefore[f] = false;
    }
    for (std::atomic<uint64_t>* counter :
         { &g_presentsSampled,  &g_presentsProcessed, &g_slotsBusy,           &g_readbacksPending,
           &g_readbackFailures, &g_createFailures,    &g_abandoned,           &g_chainBreaks,
           &g_targetChanges,    &g_composited,        &g_compositedChanged,   &g_repeats,
           &g_repeatSame,       &g_repeatOther,       &g_repeatUndecidable,   &g_repeatUnidentified,
           &g_firstRepeats,     &g_firstRepeatSame,   &g_firstRepeatOther,    &g_firstRepeatUnidentified,
           &g_overlayBound,     &g_trueDeltaUnknown,  &g_bookkeepingDisagrees })
        counter->store (0, std::memory_order_relaxed);
    for (size_t b = 0; b < kDeltaBuckets; ++b) {
        g_trueDelta[b].store (0, std::memory_order_relaxed);
        g_screenSeconds[b].store (0.0, std::memory_order_relaxed);
    }
    g_screenSecondsUnknown.store (0.0, std::memory_order_relaxed);
    g_haveHookBaseline = false;
    g_lastCalls = 0;
    g_lastDraws = 0;
    g_lastRepairs = 0;
    for (CoverageCounters& coverage : g_coverage) {
        coverage.presents.store (0, std::memory_order_relaxed);
        coverage.calls.store (0, std::memory_order_relaxed);
        coverage.draws.store (0, std::memory_order_relaxed);
        coverage.noDraws.store (0, std::memory_order_relaxed);
        coverage.repairs.store (0, std::memory_order_relaxed);
    }
    g_presentsWithRepairAfterCalls.store (0, std::memory_order_relaxed);
}

void OnPresentBegin (IDXGISwapChain* swapChain)
{
    if (!Enabled () || swapChain == nullptr)
        return;
    // A Begin without its Forward must not leak its slot; the serial gap it
    // leaves restarts the history like any other unsampled Present.
    if (g_sampling >= 0)
        g_slots[g_sampling].state = SlotState::Free;
    g_sampling = -1;
    const uint64_t serial = ++g_presentSerial;
    // What the hooks saw since the previous Present, taken whether or not this
    // Present gets a slot, so "since the previous Present" stays exact.
    const uint64_t calls = HookedArchicadCalls ();
    const uint64_t draws = HookedArchicadDraws ();
    const uint64_t repairs = ContextHookRepairs ();
    const bool coverageKnown = g_haveHookBaseline;
    const uint64_t callsSince = calls - g_lastCalls;
    const uint64_t drawsSince = draws - g_lastDraws;
    const uint64_t repairsSince = repairs - g_lastRepairs;
    g_lastCalls = calls;
    g_lastDraws = draws;
    g_lastRepairs = repairs;
    g_haveHookBaseline = true;
    if (RepairAfterCalls ())
        Bump (g_presentsWithRepairAfterCalls);
    const PresentTarget target (swapChain);
    if (!target.Valid ())
        return;
    if (g_qpcFrequency == 0) {
        LARGE_INTEGER frequency = {};
        ::QueryPerformanceFrequency (&frequency);
        g_qpcFrequency = frequency.QuadPart;
    }
    // ⚠️ OUR COPIES AND MAPS ARE NOT ARCHICAD'S WORK. Without the guard the
    // hooked copy detour would record them as host operations.
    contextstate::ScopedInjectionGuard guard;
    if (g_slots[0].staging != nullptr)
        ProcessReady (target.context);
    if (!EnsureStaging (target.device, target.desc))
        return;

    int free = -1;
    for (int i = 0; i < int (kSlots); ++i) {
        if (g_slots[i].state == SlotState::Free) {
            free = i;
            break;
        }
    }
    if (free < 0) {
        Bump (g_slotsBusy); // this Present goes unsampled; its serial gap restarts the history
        return;
    }
    Slot& slot = g_slots[free];
    slot.presentSerial = serial;
    slot.frame = -1;
    slot.coverageKnown = coverageKnown;
    slot.calls = callsSince;
    slot.draws = drawsSince;
    slot.repairs = repairsSince;
    scenecamerapairing::PresentView view;
    if (scenecamerapairing::PendingPresentView (view)) {
        slot.handoffSerial = view.handoffSerial;
        slot.imageGeneration = view.imageGeneration;
    }
    else {
        slot.handoffSerial = 0;
        slot.imageGeneration = 0;
    }
    CopyRows (target.context, target.backBuffer, slot.staging, 0);
    // The kept frames' BEFORE crop: Archicad's pixels, before any of ours.
    if (serial >= kFirstFramePresent && serial < kFirstFramePresent + kFrames) {
        const size_t f = size_t (serial - kFirstFramePresent);
        if (g_frameState[f].load (std::memory_order_relaxed) == uint32_t (FrameState::Empty) &&
            g_frameBeforeStaging[f] != nullptr) {
            uint32_t x = 0, y = 0, width = 0, height = 0;
            FrameCrop (x, y, width, height);
            D3D11_BOX box = {};
            box.left = x;
            box.right = x + width;
            box.top = y;
            box.bottom = y + height;
            box.front = 0;
            box.back = 1;
            target.context->CopySubresourceRegion (g_frameBeforeStaging[f], 0, 0, 0, 0, target.backBuffer, 0, &box);
            g_frameHasBefore[f] = true;
        }
    }
    slot.state = SlotState::Sampling;
    g_sampling = free;
}

void OnPresentForward (IDXGISwapChain* swapChain)
{
    if (!Enabled () || swapChain == nullptr || g_sampling < 0)
        return;
    Slot& slot = g_slots[g_sampling];
    g_sampling = -1;
    const PresentTarget target (swapChain);
    if (!target.Valid () || target.desc.Width != g_targetWidth || target.desc.Height != g_targetHeight ||
        uint32_t (target.desc.Format) != g_targetFormat) {
        Bump (g_abandoned);
        slot.state = SlotState::Free;
        return;
    }
    contextstate::ScopedInjectionGuard guard;
    CopyRows (target.context, target.backBuffer, slot.staging, uint32_t (kRows));
    scenecamerapairing::PresentView view;
    const bool known = scenecamerapairing::PendingPresentView (view);
    slot.cameraBound = known && view.cameraBound;
    slot.cameraImageGeneration = known ? view.cameraImageGeneration : 0;
    LARGE_INTEGER now = {};
    ::QueryPerformanceCounter (&now);
    slot.qpc = now.QuadPart;

    // A few consecutive presented frames, as pixels, after all of our drawing.
    if (slot.presentSerial >= kFirstFramePresent && slot.presentSerial < kFirstFramePresent + kFrames) {
        const size_t f = size_t (slot.presentSerial - kFirstFramePresent);
        if (g_frameState[f].load (std::memory_order_relaxed) == uint32_t (FrameState::Empty) &&
            g_frameStaging[f] != nullptr) {
            FrameInfo& info = g_frameInfo[f];
            FrameCrop (info.x, info.y, info.width, info.height);
            D3D11_BOX box = {};
            box.left = info.x;
            box.right = info.x + info.width;
            box.top = info.y;
            box.bottom = info.y + info.height;
            box.front = 0;
            box.back = 1;
            target.context->CopySubresourceRegion (g_frameStaging[f], 0, 0, 0, 0, target.backBuffer, 0, &box);
            if (g_firstFrameQpc == 0)
                g_firstFrameQpc = slot.qpc;
            info.presentSerial = slot.presentSerial;
            info.seconds = g_qpcFrequency > 0 ? double (slot.qpc - g_firstFrameQpc) / double (g_qpcFrequency) : 0.0;
            info.handoffSerial = slot.handoffSerial;
            info.imageGeneration = slot.imageGeneration;
            info.cameraImageGeneration = slot.cameraImageGeneration;
            info.cameraBound = slot.cameraBound;
            g_frameState[f].store (uint32_t (FrameState::Submitted), std::memory_order_relaxed);
            slot.frame = int (f);
        }
    }
    slot.state = SlotState::Submitted;
    Bump (g_presentsSampled);
}

Stats GetStats ()
{
    Stats stats;
    stats.enabled = Enabled ();
    stats.presentsSampled = g_presentsSampled.load (std::memory_order_relaxed);
    stats.presentsProcessed = g_presentsProcessed.load (std::memory_order_relaxed);
    stats.slotsBusy = g_slotsBusy.load (std::memory_order_relaxed);
    stats.readbacksPending = g_readbacksPending.load (std::memory_order_relaxed);
    stats.readbackFailures = g_readbackFailures.load (std::memory_order_relaxed);
    stats.createFailures = g_createFailures.load (std::memory_order_relaxed);
    stats.abandoned = g_abandoned.load (std::memory_order_relaxed);
    stats.chainBreaks = g_chainBreaks.load (std::memory_order_relaxed);
    stats.targetChanges = g_targetChanges.load (std::memory_order_relaxed);
    stats.composited = g_composited.load (std::memory_order_relaxed);
    stats.compositedChanged = g_compositedChanged.load (std::memory_order_relaxed);
    stats.repeats = g_repeats.load (std::memory_order_relaxed);
    stats.repeatSame = g_repeatSame.load (std::memory_order_relaxed);
    stats.repeatOther = g_repeatOther.load (std::memory_order_relaxed);
    stats.repeatUndecidable = g_repeatUndecidable.load (std::memory_order_relaxed);
    stats.repeatUnidentified = g_repeatUnidentified.load (std::memory_order_relaxed);
    stats.firstRepeats = g_firstRepeats.load (std::memory_order_relaxed);
    stats.firstRepeatSame = g_firstRepeatSame.load (std::memory_order_relaxed);
    stats.firstRepeatOther = g_firstRepeatOther.load (std::memory_order_relaxed);
    stats.firstRepeatUnidentified = g_firstRepeatUnidentified.load (std::memory_order_relaxed);
    stats.overlayBound = g_overlayBound.load (std::memory_order_relaxed);
    for (size_t b = 0; b < kDeltaBuckets; ++b) {
        stats.trueDelta[b] = g_trueDelta[b].load (std::memory_order_relaxed);
        stats.screenSeconds[b] = g_screenSeconds[b].load (std::memory_order_relaxed);
    }
    stats.trueDeltaUnknown = g_trueDeltaUnknown.load (std::memory_order_relaxed);
    stats.screenSecondsUnknown = g_screenSecondsUnknown.load (std::memory_order_relaxed);
    stats.bookkeepingDisagrees = g_bookkeepingDisagrees.load (std::memory_order_relaxed);
    for (size_t f = 0; f < kFrames; ++f) {
        const uint32_t state = g_frameState[f].load (std::memory_order_acquire);
        if (state == uint32_t (FrameState::Ready) || state == uint32_t (FrameState::Written))
            ++stats.framesReady;
    }
    for (size_t r = 0; r < kRelations; ++r) {
        const CoverageCounters& coverage = g_coverage[r];
        stats.coverage[r].presents = coverage.presents.load (std::memory_order_relaxed);
        stats.coverage[r].calls = coverage.calls.load (std::memory_order_relaxed);
        stats.coverage[r].draws = coverage.draws.load (std::memory_order_relaxed);
        stats.coverage[r].noDraws = coverage.noDraws.load (std::memory_order_relaxed);
        stats.coverage[r].repairs = coverage.repairs.load (std::memory_order_relaxed);
    }
    stats.presentsWithRepairAfterCalls = g_presentsWithRepairAfterCalls.load (std::memory_order_relaxed);
    for (size_t i = 0; i < kHookSlots; ++i) {
        const uint64_t now = ContextSlotRepairs (ContextSlot (i));
        stats.slotRepairs[i] = now >= g_slotRepairBaseline[i] ? now - g_slotRepairBaseline[i] : now;
    }
    return stats;
}

const char* HookSlotName (size_t slot)
{
    return slot < kHookSlots ? ContextSlotName (ContextSlot (slot)) : "?";
}

size_t WriteFrames (uint64_t epoch, FrameInfo* out, size_t capacity)
{
    if (out == nullptr)
        return 0;
    size_t copied = 0;
    std::wstring directory;
    for (size_t f = 0; f < kFrames && copied < capacity; ++f) {
        const uint32_t state = g_frameState[f].load (std::memory_order_acquire);
        if (state != uint32_t (FrameState::Ready) && state != uint32_t (FrameState::Written))
            continue;
        FrameInfo& info = g_frameInfo[f];
        if (state == uint32_t (FrameState::Ready)) {
            if (directory.empty ())
                directory = FramesDirectory ();
            if (!directory.empty () && !g_framePixels[f].empty ()) {
                const std::wstring path = directory + L"\\presented_e" + std::to_wstring (epoch) + L"_p" +
                                          std::to_wstring (info.presentSerial) + L".bmp";
                if (WriteBmp (path, g_framePixels[f].data (), info.width, info.height))
                    info.path = Utf8 (path);
                if (g_frameHasBefore[f] && !g_frameBeforePixels[f].empty ()) {
                    const std::wstring before = directory + L"\\presented_e" + std::to_wstring (epoch) + L"_p" +
                                                std::to_wstring (info.presentSerial) + L"_before.bmp";
                    if (WriteBmp (before, g_frameBeforePixels[f].data (), info.width, info.height))
                        info.pathBefore = Utf8 (before);
                }
            }
            g_frameState[f].store (uint32_t (FrameState::Written), std::memory_order_relaxed);
        }
        out[copied++] = info;
    }
    return copied;
}

} // namespace presentedcontent
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
