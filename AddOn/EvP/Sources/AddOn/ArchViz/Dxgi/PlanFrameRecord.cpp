// ArchViz/Dxgi/PlanFrameRecord -- the Present half of the floor-plan frame record.
// Bound by private/docs/architecture/diligent/OVERLAY-INVARIANTS.md: observation
// only, GPU copies under ScopedInjectionGuard, DO_NOT_WAIT readback, no locks and no
// heap on the Present path. See the header for what is recorded and why.

#include "ArchViz/Dxgi/PlanFrameRecord.hpp"

#include "ArchViz/Dxgi/ContextStateTracker.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace planframes {

namespace {

enum class FrameState : uint32_t { Empty = 0, Submitted = 1, Ready = 2, Failed = 3 };
enum class SlotState : uint32_t { Free = 0, Submitted = 1 };

// One crop on its way to the CPU. Present thread only, under `g_targetBusy`.
struct Slot {
    ID3D11Texture2D* staging = nullptr;
    SlotState state = SlotState::Free;
    int frame = -1;
    uint32_t width = 0; // the copied box, physical pixels
    uint32_t height = 0;
};

std::atomic<bool> g_armed { false };
std::atomic<bool> g_capturing { false };
std::atomic<uint64_t> g_target { 0 };
uint32_t g_mainThread = 0;

// The main thread, as it last published itself. Depth and message travel in one
// word so a reader on another thread never pairs one message's depth with another's.
std::atomic<uint64_t> g_canvasState { 0 }; // depth << 32 | message
std::atomic<uint64_t> g_canvasSerial { 0 };
std::atomic<uint64_t> g_latestSample { 0 };
std::atomic<uint64_t> g_retrievedMessage { 0 };
std::atomic<uint64_t> g_retrievedWindow { 0 };
std::atomic<uint64_t> g_retrievedSerial { 0 };
std::atomic<PresentReader> g_presentReader { nullptr };

// ⚠️ ALLOCATED BY Arm ON THE MAIN THREAD AND FREED BY Release, both only while no
// detour can be inside OnPresent: Arm runs before the Present hook is installed
// and Release after its removal has drained every call in flight.
std::vector<PresentRecord> g_records;
std::unique_ptr<std::atomic<uint32_t>[]> g_recordWritten;
std::vector<uint8_t> g_framePixels;
std::vector<FrameInfo> g_frameInfo;
std::unique_ptr<std::atomic<uint32_t>[]> g_frameState;

std::atomic<uint64_t> g_reserved { 0 };
std::atomic<uint64_t> g_dropped { 0 };
std::atomic<uint64_t> g_targetPresents { 0 };
std::atomic<uint64_t> g_framesSubmitted { 0 };
std::atomic<uint64_t> g_framesReady { 0 };
std::atomic<uint64_t> g_slotsBusy { 0 };
std::atomic<uint64_t> g_readbacksPending { 0 };
std::atomic<uint64_t> g_readbackFailures { 0 };
std::atomic<uint64_t> g_createFailures { 0 };
std::atomic<uint64_t> g_unsupportedFormat { 0 };
std::atomic<uint64_t> g_targetChanges { 0 };
std::atomic<uint32_t> g_format { 0 };

// ⚠️ ONE THREAD AT A TIME ON THE TARGET, AND NOBODY WAITS FOR IT. A swap chain is
// presented from one thread in practice; this makes it a guarantee without a
// lock. A second thread finding it taken records its Present and keeps no frame.
std::atomic<bool> g_targetBusy { false };
Slot g_slots[kStagingSlots];
uint32_t g_stagingWidth = 0;
uint32_t g_stagingFormat = 0;
bool g_createFailed = false;
uint32_t g_nextFrame = 0;
uint32_t g_targetSerial = 0;

void Bump (std::atomic<uint64_t>& counter)
{
    counter.fetch_add (1, std::memory_order_relaxed);
}

int64_t QpcNow ()
{
    LARGE_INTEGER now = {};
    ::QueryPerformanceCounter (&now);
    return now.QuadPart;
}

int64_t QpcFrequency ()
{
    static const int64_t frequency = [] () {
        LARGE_INTEGER value = {};
        ::QueryPerformanceFrequency (&value);
        return value.QuadPart;
    }();
    return frequency;
}

// Formats whose first three bytes are colour: 8-bit BGRA/BGRX and RGBA, linear or
// sRGB. Anything else is counted, not guessed at.
bool Bgra (uint32_t format)
{
    return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8X8_UNORM ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

bool Rgba (uint32_t format)
{
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
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

void ReleaseStaging ()
{
    for (Slot& slot : g_slots) {
        if (slot.staging != nullptr) {
            slot.staging->Release ();
            slot.staging = nullptr;
        }
        // A copy in flight into a released texture is gone; its frame never arrives.
        if (slot.state == SlotState::Submitted && slot.frame >= 0 && g_frameState != nullptr)
            g_frameState[size_t (slot.frame)].store (uint32_t (FrameState::Failed), std::memory_order_release);
        slot.state = SlotState::Free;
        slot.frame = -1;
    }
    g_stagingWidth = 0;
    g_stagingFormat = 0;
}

// PRESENT THREAD. Staging for this buffer's format; recreated if the format
// changes, which drops whatever was in flight.
bool EnsureStaging (ID3D11Device* device, uint32_t format)
{
    if (g_createFailed)
        return false;
    if (g_slots[0].staging != nullptr && g_stagingFormat == format)
        return true;
    if (g_slots[0].staging != nullptr)
        Bump (g_targetChanges);
    ReleaseStaging ();
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = kCropWidth;
    desc.Height = kCropHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT (format);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bool ok = true;
    for (Slot& slot : g_slots)
        ok = ok && SUCCEEDED (device->CreateTexture2D (&desc, nullptr, &slot.staging));
    if (!ok) {
        ReleaseStaging ();
        g_createFailed = true;
        Bump (g_createFailures);
        return false;
    }
    g_stagingWidth = kCropWidth;
    g_stagingFormat = format;
    return true;
}

// PRESENT THREAD. One mapped crop into its frame: a 2x2 average of luminance.
void Convert (const D3D11_MAPPED_SUBRESOURCE& mapped, const Slot& slot, uint32_t format, uint8_t* out)
{
    const bool bgra = Bgra (format);
    const uint32_t width = slot.width / kDownsample;
    const uint32_t height = slot.height / kDownsample;
    const uint8_t* base = static_cast<const uint8_t*> (mapped.pData);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row0 = base + size_t (2 * y) * mapped.RowPitch;
        const uint8_t* row1 = row0 + mapped.RowPitch;
        uint8_t* target = out + size_t (y) * kFrameWidth;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* p[4] = { row0 + 8 * x, row0 + 8 * x + 4, row1 + 8 * x, row1 + 8 * x + 4 };
            uint32_t sum = 0;
            for (const uint8_t* q : p) {
                const uint32_t r = bgra ? q[2] : q[0];
                const uint32_t g = q[1];
                const uint32_t b = bgra ? q[0] : q[2];
                sum += 77u * r + 150u * g + 29u * b;
            }
            target[x] = uint8_t ((sum + 512u) >> 10); // /256 for the weights, /4 for the average
        }
    }
}

// PRESENT THREAD. Read back finished crops, oldest frame first. Never waits.
void ReadBack (ID3D11DeviceContext* context)
{
    for (int budget = 0; budget < 4; ++budget) {
        Slot* oldest = nullptr;
        for (Slot& slot : g_slots) {
            if (slot.state == SlotState::Submitted && (oldest == nullptr || slot.frame < oldest->frame))
                oldest = &slot;
        }
        if (oldest == nullptr)
            return;
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT hr = context->Map (oldest->staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            Bump (g_readbacksPending);
            return;
        }
        const size_t frame = size_t (oldest->frame);
        if (FAILED (hr) || mapped.pData == nullptr) {
            Bump (g_readbackFailures);
            g_frameState[frame].store (uint32_t (FrameState::Failed), std::memory_order_release);
        }
        else {
            Convert (mapped, *oldest, g_stagingFormat,
                     g_framePixels.data () + frame * size_t (kFrameWidth) * kFrameHeight);
            context->Unmap (oldest->staging, 0);
            g_frameState[frame].store (uint32_t (FrameState::Ready), std::memory_order_release);
            Bump (g_framesReady);
        }
        oldest->state = SlotState::Free;
        oldest->frame = -1;
    }
}

// PRESENT THREAD, the target chain, under `g_targetBusy`.
void CaptureTarget (IDXGISwapChain* swapChain, bool capturing, int64_t presentIndex, PresentRecord& record)
{
    const PresentTarget target (swapChain);
    if (!target.Valid ())
        return;
    contextstate::ScopedInjectionGuard guard;
    ReadBack (target.context);
    if (!capturing || presentIndex < 0 || g_nextFrame >= kMaxFrames)
        return;

    const uint32_t format = uint32_t (target.desc.Format);
    g_format.store (format, std::memory_order_relaxed);
    // ⚠️ A MULTISAMPLED BUFFER CANNOT BE COPIED TO STAGING, and a format whose bytes
    // are not 8-bit colour cannot be read as grey. Both are counted, never guessed.
    if ((!Bgra (format) && !Rgba (format)) || target.desc.SampleDesc.Count != 1) {
        Bump (g_unsupportedFormat);
        return;
    }
    if (!EnsureStaging (target.device, format))
        return;
    Slot* free = nullptr;
    for (Slot& slot : g_slots) {
        if (slot.state == SlotState::Free) {
            free = &slot;
            break;
        }
    }
    if (free == nullptr) {
        Bump (g_slotsBusy);
        return;
    }
    const uint32_t width = std::min (kCropWidth, target.desc.Width) / kDownsample * kDownsample;
    const uint32_t height = std::min (kCropHeight, target.desc.Height) / kDownsample * kDownsample;
    if (width < kDownsample || height < kDownsample)
        return;
    const uint32_t x = (target.desc.Width - width) / 2;
    const uint32_t y = (target.desc.Height - height) / 2;
    D3D11_BOX box = {};
    box.left = x;
    box.right = x + width;
    box.top = y;
    box.bottom = y + height;
    box.front = 0;
    box.back = 1;
    target.context->CopySubresourceRegion (free->staging, 0, 0, 0, 0, target.backBuffer, 0, &box);

    const uint32_t frame = g_nextFrame++;
    free->state = SlotState::Submitted;
    free->frame = int (frame);
    free->width = width;
    free->height = height;
    FrameInfo& info = g_frameInfo[frame];
    info.presentIndex = uint32_t (presentIndex);
    info.targetSerial = record.targetSerial;
    info.qpc = record.qpc;
    info.cropX = x;
    info.cropY = y;
    info.width = width / kDownsample;
    info.height = height / kDownsample;
    info.bufferWidth = target.desc.Width;
    info.bufferHeight = target.desc.Height;
    info.format = format;
    g_frameState[frame].store (uint32_t (FrameState::Submitted), std::memory_order_release);
    record.frame = int32_t (frame);
    Bump (g_framesSubmitted);
}

} // namespace

void PublishCanvasMessage (uint32_t depth, uint32_t message, uint64_t serial)
{
    g_canvasSerial.store (serial, std::memory_order_relaxed);
    g_canvasState.store ((uint64_t (depth) << 32) | uint64_t (message), std::memory_order_release);
}

void PublishLatestSample (uint64_t serial)
{
    g_latestSample.store (serial, std::memory_order_release);
}

void SetPresentReader (PresentReader reader)
{
    g_presentReader.store (reader, std::memory_order_release);
}

void PublishRetrievedMessage (uint32_t message, uint64_t window, uint64_t serial)
{
    g_retrievedMessage.store (message, std::memory_order_relaxed);
    g_retrievedWindow.store (window, std::memory_order_relaxed);
    g_retrievedSerial.store (serial, std::memory_order_release);
}

bool Arm (uint32_t mainThread, std::string& error)
{
    if (g_armed.load (std::memory_order_acquire)) {
        error = "the plan frame record is already armed";
        return false;
    }
    Release (true);
    try {
        g_records.assign (kMaxPresents, PresentRecord {});
        g_recordWritten.reset (new std::atomic<uint32_t>[kMaxPresents]);
        g_framePixels.assign (kMaxFrames * size_t (kFrameWidth) * kFrameHeight, 0);
        g_frameInfo.assign (kMaxFrames, FrameInfo {});
        g_frameState.reset (new std::atomic<uint32_t>[kMaxFrames]);
    }
    catch (const std::bad_alloc&) {
        Release (true);
        error = "could not allocate the frame store (" +
                std::to_string (kMaxFrames * size_t (kFrameWidth) * kFrameHeight / (1024 * 1024)) + " MB)";
        return false;
    }
    for (size_t i = 0; i < kMaxPresents; ++i)
        g_recordWritten[i].store (0, std::memory_order_relaxed);
    for (size_t i = 0; i < kMaxFrames; ++i)
        g_frameState[i].store (uint32_t (FrameState::Empty), std::memory_order_relaxed);

    // Every Start resets what every Stop leaves behind (§8).
    g_mainThread = mainThread;
    g_target.store (0, std::memory_order_relaxed);
    g_canvasState.store (0, std::memory_order_relaxed);
    g_canvasSerial.store (0, std::memory_order_relaxed);
    g_latestSample.store (0, std::memory_order_relaxed);
    g_retrievedMessage.store (0, std::memory_order_relaxed);
    g_retrievedWindow.store (0, std::memory_order_relaxed);
    g_retrievedSerial.store (0, std::memory_order_relaxed);
    for (std::atomic<uint64_t>* counter :
         { &g_reserved, &g_dropped, &g_targetPresents, &g_framesSubmitted, &g_framesReady, &g_slotsBusy,
           &g_readbacksPending, &g_readbackFailures, &g_createFailures, &g_unsupportedFormat, &g_targetChanges })
        counter->store (0, std::memory_order_relaxed);
    g_format.store (0, std::memory_order_relaxed);
    g_targetBusy.store (false, std::memory_order_relaxed);
    g_createFailed = false;
    g_nextFrame = 0;
    g_targetSerial = 0;
    g_capturing.store (true, std::memory_order_release);
    g_armed.store (true, std::memory_order_release);
    return true;
}

void SetTarget (uint64_t swapChain)
{
    g_target.store (swapChain, std::memory_order_release);
}

uint64_t Target ()
{
    return g_target.load (std::memory_order_acquire);
}

void StopCapturing ()
{
    g_capturing.store (false, std::memory_order_release);
}

void Disarm ()
{
    g_capturing.store (false, std::memory_order_release);
    g_armed.store (false, std::memory_order_release);
}

bool Armed ()
{
    return g_armed.load (std::memory_order_acquire);
}

Stats GetStats ()
{
    Stats stats;
    stats.armed = g_armed.load (std::memory_order_acquire);
    stats.capturing = g_capturing.load (std::memory_order_acquire);
    stats.target = g_target.load (std::memory_order_acquire);
    const uint64_t reserved = g_reserved.load (std::memory_order_relaxed);
    stats.presents = std::min<uint64_t> (reserved, kMaxPresents);
    stats.presentsDropped = g_dropped.load (std::memory_order_relaxed);
    stats.targetPresents = g_targetPresents.load (std::memory_order_relaxed);
    stats.framesSubmitted = g_framesSubmitted.load (std::memory_order_relaxed);
    stats.framesReady = g_framesReady.load (std::memory_order_relaxed);
    stats.slotsBusy = g_slotsBusy.load (std::memory_order_relaxed);
    stats.readbacksPending = g_readbacksPending.load (std::memory_order_relaxed);
    stats.readbackFailures = g_readbackFailures.load (std::memory_order_relaxed);
    stats.createFailures = g_createFailures.load (std::memory_order_relaxed);
    stats.unsupportedFormat = g_unsupportedFormat.load (std::memory_order_relaxed);
    stats.targetChanges = g_targetChanges.load (std::memory_order_relaxed);
    stats.format = g_format.load (std::memory_order_relaxed);
    return stats;
}

size_t CopyPresents (PresentRecord* out, size_t capacity)
{
    if (out == nullptr || g_recordWritten == nullptr || g_armed.load (std::memory_order_acquire))
        return 0;
    const size_t count = size_t (std::min<uint64_t> (g_reserved.load (std::memory_order_acquire), kMaxPresents));
    size_t copied = 0;
    for (size_t i = 0; i < count && copied < capacity; ++i) {
        if (g_recordWritten[i].load (std::memory_order_acquire) == 0)
            continue;
        out[copied++] = g_records[i];
    }
    return copied;
}

size_t FrameCount ()
{
    if (g_frameState == nullptr || g_armed.load (std::memory_order_acquire))
        return 0;
    size_t ready = 0;
    for (size_t i = 0; i < kMaxFrames; ++i)
        if (g_frameState[i].load (std::memory_order_acquire) == uint32_t (FrameState::Ready))
            ++ready;
    return ready;
}

bool CopyFrame (size_t index, FrameInfo& info, const uint8_t*& pixels)
{
    if (g_frameState == nullptr || index >= kMaxFrames || g_armed.load (std::memory_order_acquire))
        return false;
    if (g_frameState[index].load (std::memory_order_acquire) != uint32_t (FrameState::Ready))
        return false;
    info = g_frameInfo[index];
    pixels = g_framePixels.data () + index * size_t (kFrameWidth) * kFrameHeight;
    return true;
}

void Release (bool memory)
{
    ReleaseStaging ();
    if (!memory)
        return;
    std::vector<PresentRecord> ().swap (g_records);
    g_recordWritten.reset ();
    std::vector<uint8_t> ().swap (g_framePixels);
    std::vector<FrameInfo> ().swap (g_frameInfo);
    g_frameState.reset ();
}

void OnPresent (IDXGISwapChain* swapChain, bool present1, uint32_t syncInterval, uint32_t flags, uint32_t dirtyRects,
                bool scroll, int32_t scrollX, int32_t scrollY)
{
    if (!g_armed.load (std::memory_order_acquire) || swapChain == nullptr)
        return;
    const uint64_t chain = uint64_t (uintptr_t (swapChain));
    const bool isTarget = chain == g_target.load (std::memory_order_acquire);
    const bool capturing = g_capturing.load (std::memory_order_acquire);
    // Closing: only the target's readbacks still have work to do.
    if (!capturing && !isTarget)
        return;

    PresentRecord record;
    record.qpc = QpcNow ();
    record.chain = chain;
    record.thread = uint32_t (::GetCurrentThreadId ());
    record.mainThread = record.thread == g_mainThread;
    record.flags = flags;
    record.syncInterval = syncInterval;
    record.present1 = present1;
    record.dirtyRects = dirtyRects;
    record.scroll = scroll;
    record.scrollX = scrollX;
    record.scrollY = scrollY;
    const uint64_t canvas = g_canvasState.load (std::memory_order_acquire);
    record.canvasDepth = uint32_t (canvas >> 32);
    record.canvasMessage = uint32_t (canvas & 0xFFFFFFFFu);
    record.canvasSerial = g_canvasSerial.load (std::memory_order_relaxed);
    record.latestSample = g_latestSample.load (std::memory_order_acquire);
    record.retrievedSerial = g_retrievedSerial.load (std::memory_order_acquire);
    record.retrievedMessage = uint32_t (g_retrievedMessage.load (std::memory_order_relaxed));
    record.retrievedWindow = g_retrievedWindow.load (std::memory_order_relaxed);
    record.target = isTarget;
    // The read at the Present (see SetPresentReader): the plan's chain, capturing,
    // and the main thread -- all three, every call. Before the crop, so the read and
    // the pixels describe one moment.
    if (isTarget && capturing && record.mainThread) {
        const PresentReader reader = g_presentReader.load (std::memory_order_acquire);
        if (reader != nullptr)
            record.atPresent = reader ();
    }

    int64_t index = -1;
    if (capturing) {
        const uint64_t slot = g_reserved.fetch_add (1, std::memory_order_relaxed);
        if (slot < kMaxPresents)
            index = int64_t (slot);
        else
            Bump (g_dropped);
    }

    if (isTarget) {
        bool expected = false;
        if (g_targetBusy.compare_exchange_strong (expected, true, std::memory_order_acquire)) {
            if (capturing) {
                record.targetSerial = ++g_targetSerial;
                Bump (g_targetPresents);
            }
            const int64_t began = QpcNow ();
            CaptureTarget (swapChain, capturing, index, record);
            const int64_t frequency = QpcFrequency ();
            if (frequency > 0)
                record.tookUs = uint32_t ((QpcNow () - began) * 1000000 / frequency);
            g_targetBusy.store (false, std::memory_order_release);
        }
    }

    if (index >= 0) {
        g_records[size_t (index)] = record;
        g_recordWritten[size_t (index)].store (1, std::memory_order_release);
    }
}

} // namespace planframes
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
